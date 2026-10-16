#include "talcite/promise.hpp"

#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "talcite/loop.hpp"

namespace talcite {

// The model in one sentence: work lasts as long as somebody wants it. Every handle of a promise is
// one owner of a node (`detail::Core`); when the last owner leaves, the node ends with a
// cancellation at once and its work is asked to stop. A derived node (`then`, `recover`, `finally`,
// `on`, `timeout`, `outcome`, a join, a view of a shared promise) is itself the owner of the nodes it
// was made from, so the tail of a chain owns the whole chain, and a node several consumers share
// lives while any of them does.
//
// An ending and the end of the work are two moments. The ending (a value, an error, a cancellation)
// happens once, and from then on consumers see it. The work may still go on after a cancellation:
// a body that has not noticed the stop yet, a producer still holding its `Settle`. The node counts
// what has to finish (`work_`: its own work, its predecessors, a callback running, an adopted
// promise), and only when that count reaches zero after the ending is the node finished. `finally`
// waits for that moment, which is what makes it fit for cleaning up. A reaction that may run a
// callback (`then`, `recover`, and `on`, which hands the ending on elsewhere) holds its node first
// and only then looks whether the node still runs: an ending from `Settle` sends the reactions and
// the finish of the predecessor to the io lane apart, so they may run on two threads at once, and a
// look before the hold would let a cancellation finish the node, and `finally` run, while the
// callback is just starting.
//
// Nothing internal recurses: releasing a chain, delivering an ending and announcing that work has
// finished are all steps of one trampoline per thread (`stepped`), so a chain of any length runs at
// a constant depth of the stack. User code (callbacks, coroutines) runs only inside such steps, never
// under a lock of the library, never inside `cancel()`, a destructor or `Settle`. An ending reported
// from an arbitrary place (`Settle`, the clock, a stop) is delivered on the io lane of the pool.

// The share of the work done, from 0 to 1; zero when the whole is unknown.
double Progress::fraction() const {
    if (whole == 0) return 0.0;

    const double made = static_cast<double>(done) / static_cast<double>(whole);

    return made < 0.0 ? 0.0 : (made > 1.0 ? 1.0 : made);
}

Overdue::Overdue() : Trouble("Timed out.") {}

Broken::Broken() : Trouble("The promise was abandoned unsettled.") {}

namespace detail {

namespace {

// The queue of the trampoline running on this thread, or null when none runs. It lies on the stack
// of the outermost `stepped`; a thread-local object with a destructor would be destroyed at thread
// exit before others that may still step.
thread_local std::deque<Step>* steps = nullptr;

// Where errors nobody will receive go (`lost`).
std::mutex losing;
std::function<void(const std::exception&)> sink;

// Withdraws the queue of this thread's trampoline however the loop ends.
class Stepping {
public:
    // Makes `mine` the queue of this thread's trampoline.
    explicit Stepping(std::deque<Step>& mine) { steps = &mine; }

    // Leaves the thread without a trampoline again.
    ~Stepping() { steps = nullptr; }

    Stepping(const Stepping&) = delete;
    Stepping& operator=(const Stepping&) = delete;
    Stepping(Stepping&&) = delete;
    Stepping& operator=(Stepping&&) = delete;
};

// Runs the steps of `mine` until it is empty. A step that throws loses only itself: what it threw
// has nobody to go to, since every step that runs user code settles its own node with what the code
// threw, and an internal step throws only when memory runs out.
void ran(std::deque<Step>& mine) {
    while (!mine.empty()) {
        Step next = std::move(mine.front());
        mine.pop_front();

        try {
            next();
        } catch (...) {
            lose(std::current_exception());
        }
    }
}

}

// Runs `step` now when no trampoline runs on this thread, and otherwise puts it at the end of the
// queue of the one that runs, to be done after the current step returns.
void stepped(Step step) {
    if (steps != nullptr) {
        steps->push_back(std::move(step));

        return;
    }

    std::deque<Step> mine;
    mine.push_back(std::move(step));

    const Stepping held{mine};

    ran(mine);
}

// Whether a trampoline runs on this thread, that is, whether a step queued now runs only after the
// caller has returned. A callback for something already ended must not run inside the call that
// registers it, so outside a trampoline it goes to the io lane instead (`away`).
bool stepping() { return steps != nullptr; }

// Runs what this thread's trampoline still has queued, before the thread blocks. A blocking wait
// inside a step would otherwise wait for a step queued behind itself on its own thread, for ever.
void drained() {
    if (steps == nullptr) return;

    std::deque<Step> mine;
    mine.swap(*steps);

    ran(mine);
}

// Hands `what` to the io lane of the pool, as a fresh trampoline there. When the pool no longer takes
// work (the program is ending), it runs here and now: late, but not lost.
void away(std::function<void()> what) {
    auto shared = std::make_shared<std::function<void()>>(std::move(what));

    if (io().post([shared] { stepped([shared] { (*shared)(); }); })) return;

    stepped([shared] { (*shared)(); });
}

// Gives an error that nobody will receive to the sink set by `lost`, on the io lane; an exception
// from the sink itself is swallowed, since there is nowhere further to give it.
void lose(std::exception_ptr why) {
    if (!why) return;

    std::function<void(const std::exception&)> to;

    {
        const std::lock_guard<std::mutex> held{losing};

        to = sink;
    }

    if (!to) return;

    (void)io().post([to, why] {
        try {
            std::rethrow_exception(why);
        } catch (const std::exception& one) {
            try {
                to(one);
            } catch (...) {
            }
        } catch (...) {
        }
    });
}

// The exception being handled, as an error a promise may hold: anything that is not a
// `std::exception` becomes `Rejected`, so that every consumer can rely on `std::exception`.
std::exception_ptr caught() {
    try {
        throw;
    } catch (const std::exception&) {
        return std::current_exception();
    } catch (...) {
        return std::make_exception_ptr(Rejected{"Unknown reason for rejection."});
    }
}

// Whether `why` is a cancellation; a producer or a body that gives one ends its node cancelled,
// not failed.
bool quitting(const std::exception_ptr& why) {
    if (!why) return false;

    try {
        std::rethrow_exception(why);
    } catch (const Cancelled&) {
        return true;
    } catch (...) {
        return false;
    }
}

// The cancellation a cancelled node holds, for whoever asks for its reason.
std::exception_ptr quitted() { return std::make_exception_ptr(Cancelled{}); }

// The stop of the work running on this thread (a body of `run`, a resumed coroutine), or null.
const Stop*& current_stop() {
    thread_local const Stop* now = nullptr;

    return now;
}

// Whether this thread is the one of the place behind `home`: it is when the loop standing on it is
// that place. Work for a loop runs only on the loop's thread, so code that finishes there may go on
// at once; on any other thread it must go to the place. Other places (the pool's lanes, a manual
// place) have no thread of their own to tell, so for them the answer is no.
bool homed(const std::shared_ptr<Gate>& home) {
    const Loop* mine = Loop::current();

    return mine != nullptr && mine->gate() == home;
}

// The loop of this thread, for `here`; without one it is a mistake of the caller.
Place& here_loop() {
    Loop* where = Loop::current();

    if (where == nullptr) throw std::logic_error("talcite::Promise::here: no loop on this thread");

    return *where;
}

// Gives up this claim: one owner less, and the reaction registered with it is withdrawn.
void Claim::release() {
    if (core_ == nullptr) return;

    const std::shared_ptr<Core> core = std::move(core_);
    const std::uint64_t reaction = std::exchange(reaction_, 0);

    core->released(reaction);
}

// A node whose work counts `work` things that must finish: one for a producer (a body, a
// `Settle`, a coroutine), none for a derived node, which counts its predecessors as it links them.
Core::Core(std::size_t work) : work_(work) {}

Core::~Core() = default;

// One more owner of `core`.
Claim Core::claimed(const std::shared_ptr<Core>& core) {
    {
        const std::lock_guard<std::mutex> held{core->guard_};

        ++core->owners_;
    }

    return Claim{core, 0};
}

// Registers `reaction` for the ending of this node, tied to `claim`: releasing the claim withdraws
// it. When the node has already ended, the reaction is delivered at once, but never inside this
// call: after the current step, or on the io lane outside a trampoline.
void Core::react(Claim& claim, std::shared_ptr<Reaction> reaction) {
    {
        const std::lock_guard<std::mutex> held{guard_};

        if (ending_ == Ending::running) {
            claim.reaction_ = ++next_;
            reactions_.emplace(claim.reaction_, std::move(reaction));

            return;
        }
    }

    if (stepping()) {
        stepped([reaction] { reaction->ended(); });

        return;
    }

    away([reaction] { reaction->ended(); });
}

// Keeps a claim on a predecessor, to be released when this node ends or is cancelled. A node that
// has already ended releases it at once.
void Core::keep(Claim claim) {
    {
        const std::lock_guard<std::mutex> held{guard_};

        if (ending_ == Ending::running) {
            upstream_.push_back(std::move(claim));

            return;
        }
    }

    claim.release();
}

// Reports the progress of `whose` as this node's own until it ends.
void Core::follow(std::shared_ptr<Core> whose) {
    const std::lock_guard<std::mutex> held{guard_};

    if (ending_ == Ending::running) following_ = std::move(whose);
}

// Remembers the place the chain moved to (`on`, `here`), for a `finally` that runs after a
// cancellation: it goes back there rather than to the io lane, so that code bound to a loop's thread
// cleans up on that thread. A node without a place of its own takes its predecessor's.
void Core::dwell(std::shared_ptr<Gate> where) {
    if (where == nullptr) return;

    const std::lock_guard<std::mutex> held{guard_};

    home_ = std::move(where);
}

// The place the chain moved to before this node, or null.
std::shared_ptr<Gate> Core::home() const {
    const std::lock_guard<std::mutex> held{guard_};

    return home_;
}

// Reports the summed progress of `among` (a join) until it ends.
void Core::sum(std::vector<std::shared_ptr<Core>> among) {
    const std::lock_guard<std::mutex> held{guard_};

    if (ending_ == Ending::running) summing_ = std::move(among);
}

// Lets the work go on without an owner: leaving owners no longer stop it, and an error it ends with
// goes to `lost`.
void Core::detach() {
    std::exception_ptr why;

    {
        const std::lock_guard<std::mutex> held{guard_};

        detached_ = true;

        if (ending_ == Ending::error) why = why_;
    }

    lose(why);
}

// Marks the value as shared by many readers: it is copied out, never moved.
void Core::share() {
    const std::lock_guard<std::mutex> held{guard_};

    shared_ = true;
}

// Whether the value is shared by many readers.
bool Core::shared() const {
    const std::lock_guard<std::mutex> held{guard_};

    return shared_;
}

// How the node has ended, or `running`.
Ending Core::ending() const {
    const std::lock_guard<std::mutex> held{guard_};

    return ending_;
}

// Whether the node has not ended yet.
bool Core::running() const { return ending() == Ending::running; }

// The error the node ended with, or its cancellation.
std::exception_ptr Core::why() const {
    const std::lock_guard<std::mutex> held{guard_};

    return why_;
}

// Ends the node with an error, a cancellation when the error is one; false when it has already
// ended. Whatever is not a `std::exception` becomes `Rejected`, so every consumer can rely on it.
bool Core::failed(std::exception_ptr why, Delivery delivery) {
    if (quitting(why)) return quit(delivery);

    if (why == nullptr) {
        why = std::make_exception_ptr(Rejected{"Unknown reason for rejection."});
    } else {
        try {
            std::rethrow_exception(why);
        } catch (const std::exception&) {
        } catch (...) {
            why = std::make_exception_ptr(Rejected{"Unknown reason for rejection."});
        }
    }

    Concluded made;

    {
        const std::lock_guard<std::mutex> held{guard_};

        if (ending_ != Ending::running) return false;

        made = concluded_locked(Ending::error, std::move(why));
    }

    delivered(std::move(made), delivery);

    return true;
}

// Ends the node with a cancellation that comes from its own work, not from its owners; false when it
// has already ended.
bool Core::quit(Delivery delivery) {
    Concluded made;

    {
        const std::lock_guard<std::mutex> held{guard_};

        if (ending_ != Ending::running) return false;

        made = concluded_locked(Ending::cancelled, quitted());
    }

    delivered(std::move(made), delivery);

    return true;
}

// One more thing that has to finish before the node is finished.
void Core::hold() {
    const std::lock_guard<std::mutex> held{guard_};

    ++work_;
}

// One thing less that has to finish; the last one after the ending finishes the node.
void Core::let_go() {
    std::vector<std::function<void()>> then;

    {
        const std::lock_guard<std::mutex> held{guard_};

        if (work_ > 0) --work_;

        if (work_ == 0 && ending_ != Ending::running && !finished_) then = finishing_locked();
    }

    for (std::function<void()>& one : then) stepped(std::move(one));
}

// `let_go` on the io lane, for producers that report from an arbitrary place (`Settle`).
void Core::let_go_away() {
    away([self = shared_from_this()] { self->let_go(); });
}

// Runs `then` once the node is finished; at once when it already is, but never inside this call.
void Core::when_finished(std::function<void()> then) {
    {
        const std::lock_guard<std::mutex> held{guard_};

        if (!finished_) {
            on_finished_.push_back(std::move(then));

            return;
        }
    }

    if (stepping()) stepped(std::move(then));
    else away(std::move(then));
}

// Runs `then` once the node has ended; at once when it already has, but never inside this call.
void Core::when_ended(std::function<void()> then) {
    {
        const std::lock_guard<std::mutex> held{guard_};

        if (ending_ == Ending::running) {
            on_ended_.push_back(std::move(then));

            return;
        }
    }

    if (stepping()) stepped(std::move(then));
    else away(std::move(then));
}

// A body is about to run: false when the node has already ended (the owners left before the body
// started), and then the body must not run. Otherwise the worker's bell is lent to the node, so that
// a stop rings it while the body waits in a system call.
bool Core::begin(Bell* bell) {
    const std::lock_guard<std::mutex> held{guard_};

    if (ending_ != Ending::running) return false;

    started_ = true;
    lent_ = bell;

    return true;
}

// Whether the body has begun.
bool Core::started() const {
    const std::lock_guard<std::mutex> held{guard_};

    return started_;
}

// The body has returned: the worker's bell goes back to the worker. Under the same lock as the ring,
// so a late stop can no longer ring it for the worker's next task.
void Core::unlend() {
    const std::lock_guard<std::mutex> held{guard_};

    lent_ = nullptr;
}

// Waits at most `patience` (without limit when negative) for the stop; whether it came.
bool Core::wait_stop(std::chrono::milliseconds patience) const {
    std::unique_lock<std::mutex> held{guard_};

    const auto come = [this] { return stopped_.load(); };

    if (patience.count() < 0) {
        stirred_.wait(held, come);

        return true;
    }

    return stirred_.wait_for(held, patience, come);
}

// A descriptor (an event on Windows) that becomes readable when the work is stopped: the worker's
// bell while a body runs, otherwise a bell of the node's own, made on first use.
Heard Core::heard() {
    const std::lock_guard<std::mutex> held{guard_};

    if (lent_ != nullptr) return lent_->waited();

    if (own_ == nullptr) {
        own_ = std::make_unique<Bell>();

        if (stopped_.load()) own_->ring();
    }

    return own_->waited();
}

// Registers an internal listener for the stop, run inline when the stop comes; at once (after the
// current step) when it already has. Library code only: it runs inside `cancel()`.
std::uint64_t Core::on_stop(std::function<void()> then) {
    {
        const std::lock_guard<std::mutex> held{guard_};

        if (!stopped_.load()) {
            const std::uint64_t which = ++next_;

            on_stop_.emplace(which, std::move(then));

            return which;
        }
    }

    stepped(std::move(then));

    return 0;
}

// Withdraws an internal stop listener.
void Core::off_stop(std::uint64_t which) {
    if (which == 0) return;

    const std::lock_guard<std::mutex> held{guard_};

    on_stop_.erase(which);
}

// Registers a user's hook for the stop (`Stop::when`); it runs on the io lane, once.
std::uint64_t Core::hooked(std::function<void()> then) {
    std::uint64_t which = 0;

    {
        const std::lock_guard<std::mutex> held{guard_};

        which = ++next_;

        const bool now = stopped_.load();

        hooks_.emplace(which, Hooking{std::move(then), now ? 1 : 0, {}});

        if (!now) return which;
    }

    away([self = shared_from_this(), which] { self->hook_run(which); });

    return which;
}

// Runs the hook `which`, if it is still waiting for its turn.
void Core::hook_run(std::uint64_t which) {
    std::function<void()> then;

    {
        const std::lock_guard<std::mutex> held{guard_};

        const auto at = hooks_.find(which);

        if (at == hooks_.end() || at->second.state != 1) return;

        at->second.state = 2;
        at->second.runner = std::this_thread::get_id();

        then = at->second.then;
    }

    try {
        then();
    } catch (...) {
        lose(caught());
    }

    {
        const std::lock_guard<std::mutex> held{guard_};

        hooks_.erase(which);
    }

    stirred_.notify_all();
}

// Withdraws the hook `which`: afterwards it will not start. One running on another thread is waited
// for; one running on this thread (the hook unhooking itself) is not.
void Core::unhooked(std::uint64_t which) {
    std::unique_lock<std::mutex> held{guard_};

    for (;;) {
        const auto at = hooks_.find(which);

        if (at == hooks_.end()) return;

        if (at->second.state == 2 && at->second.runner != std::this_thread::get_id()) {
            stirred_.wait(held);

            continue;
        }

        hooks_.erase(at);

        return;
    }
}

// Waits for the ending; `running` when the stop of `also` (the work the waiting thread does itself)
// came first. What this thread's trampoline has queued runs first, so the wait never waits for a
// step queued behind itself.
Ending Core::waited(Core* also) {
    drained();

    const std::shared_ptr<Core> self = shared_from_this();

    std::uint64_t ear = 0;

    if (also != nullptr && also != this)
        ear = also->on_stop([self] {
            {
                const std::lock_guard<std::mutex> held{self->guard_};
            }

            self->stirred_.notify_all();
        });

    Ending made = Ending::running;

    {
        std::unique_lock<std::mutex> held{guard_};

        stirred_.wait(held, [&] { return ending_ != Ending::running || (also != nullptr && also->stopped()); });

        made = ending_;
    }

    if (also != nullptr) also->off_stop(ear);

    return made;
}

// Sets the progress the producer reports.
void Core::report(Progress now) {
    const std::lock_guard<std::mutex> held{guard_};

    progress_ = now;
}

// The progress of the work this node waits for: its own, the one it follows, or the sum of a join;
// complete once it has ended. Following goes along the chain without recursion.
Progress Core::progress() const {
    std::shared_ptr<const Core> at;
    const Core* now = this;

    for (;;) {
        std::shared_ptr<Core> next;
        std::vector<std::shared_ptr<Core>> among;

        {
            const std::lock_guard<std::mutex> held{now->guard_};

            if (now->ending_ != Ending::running) {
                const std::uint64_t whole = now->progress_.whole == 0 ? 1 : now->progress_.whole;

                return Progress{whole, whole};
            }

            if (!now->summing_.empty()) among = now->summing_;
            else if (now->following_ != nullptr) next = now->following_;
            else return now->progress_;
        }

        if (next != nullptr) {
            at = next;
            now = next.get();

            continue;
        }

        Progress made;

        for (const std::shared_ptr<Core>& one : among) {
            const Progress part = one->progress();

            made.done += part.done;
            made.whole += part.whole;
        }

        return made;
    }
}

// The node ends: everything that waited for the ending is taken out to be run after the lock is
// released. The caller holds the lock.
Core::Concluded Core::concluded_locked(Ending how, std::exception_ptr why) {
    ending_ = how;
    why_ = std::move(why);

    Concluded made;

    made.reactions.reserve(reactions_.size());

    for (auto& [which, one] : reactions_) made.reactions.push_back(std::move(one));

    reactions_.clear();

    made.ended.swap(on_ended_);
    made.upstream.swap(upstream_);

    following_.reset();
    summing_.clear();
    on_stop_.clear();

    for (auto at = hooks_.begin(); at != hooks_.end();) {
        if (at->second.state == 0) at = hooks_.erase(at);
        else ++at;
    }

    if (detached_ && how == Ending::error) made.lost = why_;

    if (work_ == 0 && !finished_) made.finished = finishing_locked();

    stirred_.notify_all();

    return made;
}

// Runs what an ending took out: predecessors released, reactions delivered here or on the io lane,
// listeners for the ending and for the finish.
void Core::delivered(Concluded made, Delivery delivery) {
    lose(made.lost);

    if (!made.upstream.empty()) stepped([upstream = std::move(made.upstream)]() mutable { upstream.clear(); });

    for (std::function<void()>& one : made.ended) stepped(std::move(one));

    if (delivery == Delivery::here) {
        for (const std::shared_ptr<Reaction>& one : made.reactions) stepped([one] { one->ended(); });

        for (std::function<void()>& one : made.finished) stepped(std::move(one));

        return;
    }

    if (!made.reactions.empty())
        away([reactions = std::move(made.reactions)] {
            for (const std::shared_ptr<Reaction>& one : reactions) stepped([one] { one->ended(); });
        });

    for (std::function<void()>& one : made.finished) away(std::move(one));
}

// The node is finished: its finish listeners are taken out. The caller holds the lock.
std::vector<std::function<void()>> Core::finishing_locked() {
    finished_ = true;

    std::vector<std::function<void()>> made;

    made.swap(on_finished_);

    return made;
}

// An owner leaves. The last one, while the node runs and nobody let it go on alone, ends it with a
// cancellation and stops its work: the flag, the bell, the internal listeners at once, the user's
// hooks on the io lane, the predecessors released. No user code runs here.
void Core::released(std::uint64_t reaction) {
    std::shared_ptr<Reaction> gone;
    std::vector<std::function<void()>> stops;
    std::vector<std::uint64_t> fired;
    Concluded made;
    bool ended = false;

    {
        const std::lock_guard<std::mutex> held{guard_};

        if (reaction != 0) {
            const auto at = reactions_.find(reaction);

            if (at != reactions_.end()) {
                gone = std::move(at->second);
                reactions_.erase(at);
            }
        }

        if (owners_ > 0) --owners_;

        if (owners_ == 0 && !detached_ && ending_ == Ending::running) {
            stopped_.store(true);

            if (lent_ != nullptr) lent_->ring();
            if (own_ != nullptr) own_->ring();

            for (auto& [which, one] : on_stop_) stops.push_back(std::move(one));

            on_stop_.clear();

            for (auto& [which, one] : hooks_)
                if (one.state == 0) {
                    one.state = 1;
                    fired.push_back(which);
                }

            made = concluded_locked(Ending::cancelled, quitted());
            ended = true;
        }
    }

    if (!ended) return;

    for (std::function<void()>& one : stops) stepped(std::move(one));

    for (const std::uint64_t which : fired) away([self = shared_from_this(), which] { self->hook_run(which); });

    delivered(std::move(made), Delivery::here);
}

// Ends a promise without a value.
bool Node<void>::valued(Delivery delivery) {
    Concluded made;

    {
        const std::lock_guard<std::mutex> held{guard_};

        if (ending_locked() != Ending::running) return false;

        made = concluded_locked(Ending::value, nullptr);
    }

    delivered(std::move(made), delivery);

    return true;
}

// The work a coroutine waited for was abandoned with its owners: the claim on it goes, and the
// coroutine resumes on the io lane with `Cancelled` where it waited. A resumption that has already
// begun wins.
void Suspension::stopped() {
    std::shared_ptr<Waited> now;

    {
        const std::lock_guard<std::mutex> held{guard_};

        stopped_ = true;
        now = std::move(waited_);
    }

    if (now == nullptr) return;

    Claim gone;

    {
        const std::lock_guard<std::mutex> held{now->guard};

        if (now->resumed) return;

        now->resumed = true;
        now->cancelled = true;

        gone = std::move(now->claim);
    }

    gone.release();

    away([now] { now->handle.resume(); });
}

// What a coroutine waited for has ended: it resumes here, unless a cancellation resumed it first.
void Resuming::ended() {
    {
        const std::lock_guard<std::mutex> held{waited_->guard};

        if (waited_->resumed) return;

        waited_->resumed = true;
    }

    waited_->handle.resume();
}

// Arms the deadline of `timeout`: when it passes first, the node fails with `Overdue` on the io lane
// and releases what it waited for. The deadline is withdrawn before the node can end otherwise: by
// the reaction that passes the ending on (`Timing`), and at once when the owners leave. Withdrawn
// later, after the ending is visible, it would reach for a clock its owner may already have
// destroyed.
std::uint64_t timed(const std::shared_ptr<Core>& to, Clock& clock, std::chrono::milliseconds patience) {
    const std::weak_ptr<Core> weak = to;

    const std::uint64_t which = clock.scheduled(clock.now() + patience, [weak] {
        away([weak] {
            if (const std::shared_ptr<Core> one = weak.lock()) (void)one->failed(std::make_exception_ptr(Overdue{}), Delivery::here);
        });
    });

    Clock* const whose = &clock;

    (void)to->on_stop([whose, which] { (void)whose->unscheduled(which); });

    return which;
}

// Hands the job of a body to its place. A place that refuses ends the node cancelled. A pool lane
// can also take a job back: when the owners leave before the body starts, the job leaves the queue
// at once instead of waiting for a worker to find it abandoned.
void launched(Place& where, const std::shared_ptr<Core>& node, std::function<void()> job) {
    auto* lane = dynamic_cast<Withdrawing*>(&where);

    if (lane == nullptr) {
        if (where.post(std::move(job))) return;

        (void)node->quit(Delivery::here);
        node->let_go();

        return;
    }

    const std::uint64_t which = lane->queued(std::move(job));

    if (which == 0) {
        (void)node->quit(Delivery::here);
        node->let_go();

        return;
    }

    (void)node->on_stop([gate = where.gate(), weak = std::weak_ptr<Core>{node}, which] {
        const std::shared_ptr<Core> one = weak.lock();

        if (one == nullptr || one->started()) return;

        bool gone = false;

        {
            const std::lock_guard<std::mutex> held{gate->guard};

            if (auto* taking = dynamic_cast<Withdrawing*>(gate->place)) gone = taking->withdrawn(which);
        }

        if (gone) one->let_go();
    });
}

}

// Withdraws the hook it holds before taking another.
Hook& Hook::operator=(Hook&& other) noexcept {
    if (this != &other) {
        if (const std::shared_ptr<detail::Core> core = core_.lock()) core->unhooked(which_);

        core_ = std::move(other.core_);
        which_ = std::exchange(other.which_, 0);
    }

    return *this;
}

// Withdraws the hook: afterwards it will not start.
Hook::~Hook() {
    if (which_ == 0) return;

    if (const std::shared_ptr<detail::Core> core = core_.lock()) core->unhooked(which_);
}

// Whether the work has been stopped.
bool Stop::stopped() const { return core_->stopped(); }

// Waits at most `patience` (without limit when negative) for the stop; whether it came.
bool Stop::wait(std::chrono::milliseconds patience) const { return core_->wait_stop(patience); }

// A descriptor (an event on Windows) that becomes readable when the work is stopped, to wait for
// together with one's own in `poll` or `WaitForMultipleObjects`.
Stop::Handle Stop::heard() const { return core_->heard(); }

// Runs `then` once, on the io lane, when the work is stopped; the hook withdraws it.
Hook Stop::when(std::function<void()> then) const { return Hook{core_, core_->hooked(std::move(then))}; }

// The stop of the work running on this thread, or null.
const Stop* Stop::current() { return detail::current_stop(); }

// The producer's view of a running body.
Work::Work(std::shared_ptr<detail::Core> core) : stop_(detail::Access::stop(std::move(core))) {}

// Reports the progress of the body.
void Work::progress(Progress now) const { detail::Access::core(stop_)->report(now); }

// Sets where errors nobody receives go: work let go with `detach`, a throwing `finally` with no
// owner, a throwing hook. The sink runs on the io lane.
void lost(std::function<void(const std::exception&)> sink) {
    const std::lock_guard<std::mutex> held{detail::losing};

    detail::sink = std::move(sink);
}

// A promise already fulfilled, without a value.
Promise<void> ready() {
    auto node = std::make_shared<detail::Node<void>>(0);
    Promise<void> made = detail::Access::made<void>(detail::Core::claimed(node));

    (void)node->valued(detail::Delivery::here);

    return made;
}

// All of `among`: fulfilled when every one is, failed by the first failure, cancelled by the first
// cancellation; the rest are released either way.
//
// The `all` of promises of different types, in the header, hangs the same handler on each promise.
// Under MSVC it goes through `detail::joined_each`, which hangs it on one promise at a time
// (`joined_at`), instead of a generic lambda: MSVC does not expand the function's own pack
// (`among`) inside the pack of a lambda (error C3520), which the standard allows and GCC and Clang
// build.
Promise<void> all(std::vector<Promise<void>> among) {
    auto to = std::make_shared<detail::Node<void>>(0);
    Promise<void> made = detail::Access::made<void>(detail::Core::claimed(to));

    if (among.empty()) {
        (void)to->valued(detail::Delivery::here);

        return made;
    }

    std::vector<std::shared_ptr<detail::Core>> summed;

    for (Promise<void>& one : among) summed.push_back(detail::Access::claim(one).core());

    auto left = std::make_shared<detail::Counting>(among.size());

    for (Promise<void>& one : among)
        detail::joined<void>(to, one, [to, left](detail::Node<void>& from) {
            if (!to->running()) return;

            switch (from.ending()) {
            case detail::Ending::value:
                if (left->last()) (void)to->valued(detail::Delivery::here);
                break;
            case detail::Ending::error:
                (void)to->failed(from.why(), detail::Delivery::here);
                break;
            default:
                (void)to->quit(detail::Delivery::here);
                break;
            }
        });

    to->sum(std::move(summed));

    return made;
}

}
