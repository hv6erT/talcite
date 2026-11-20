#include "tal/docs.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include "tal/executor.hpp"
#include "tal/trouble.hpp"

#include "rendering.hpp"

// Documents and pages rendered by another process: the ports `Renderer` and `Browser`, their
// chains, the adapters on any program that speaks the protocol of `rendering` and the scripted
// adapters of the trials.
//
// The adapter of a program keeps one process (`Engine`) and starts it when it is first needed. The
// process greets first; one that ends or keeps silent before its hello is an engine that is not
// there, and is not started again while the program lives, so a chain goes past it at once. One
// that ends after its hello takes every question waiting with it (`Trouble` broken), and the next
// question starts it anew; documents and views of the process that ended are lost with it. A
// question unanswered within its limit kills the process (`Trouble` overdue). Whatever the process
// says is checked before it is believed: a frame of another shape, an answer to nothing or a
// picture of another size than asked kills the process (`Trouble` malformed).
//
// The engine's own work (starting the process, reading what it says, answering its reads) runs on
// the pool of io, which outlives every loop, and holds the process's streams and the engine only
// for a moment, never the process itself: the engine alone holds that, and when the last of its
// adapters, documents and views lets it go, it kills the process (on that thread, or a moment later
// on the pool, when the engine's own work held it just then). The limits of time run on the clock
// of whoever asks.
namespace tal::docs {

namespace {

using rendering::Frame;
using rendering::Sort;

constexpr std::size_t read_at_once = std::size_t{64} << 10;

// The trouble of a process that ended, for `why`.
Trouble ended(const std::string& engine, const std::string& why) { return Trouble{Kind::broken, "The renderer " + engine + " ended: " + why}; }

// The bytes of the frame of `sort` asking `id` with `payload`; apart from the coroutines, which
// take no braced lists.
Bytes framed_as(Sort sort, std::uint32_t id, Bytes payload) {
    Frame made;

    made.sort = sort;
    made.id = id;
    made.payload = std::move(payload);

    return rendering::framed(made);
}

// Throws `Trouble` (other) for a size of no pixels or over the ceiling.
void sized(media::Size size) {
    const std::uint64_t much = std::uint64_t{size.width} * size.height;

    if (much == 0 || much > media::ceiling) throw Trouble{Kind::other, "A size of no pixels or over the ceiling."};
}

// One process of an engine as its tasks see it: the engine's name, the process's streams, the
// hello, the questions waiting for an answer and the last write, after which the next one goes
// (under `writing`). The tasks hold the run, and the engine only for a moment, never the process
// itself.
struct Run {
    std::string name;
    std::uint64_t generation = 0;
    Stream input;
    Stream output;
    Stream errors;
    std::shared_ptr<Shared<rendering::Hello>> greeting;
    Pending<rendering::Hello> hello;
    bool greeted = false;
    bool ended = false;
    std::map<std::uint32_t, Pending<Frame>> waiting;
    std::mutex writing;
    std::shared_ptr<Pending<void>> written;
};

// A place in the order of `run`'s writes, taken now: the write before it, and this one's end.
struct Turn {
    std::shared_ptr<Pending<void>> before;
    std::shared_ptr<Pending<void>> mine = std::make_shared<Pending<void>>();
};

// Takes the next place in the order of `run`'s writes.
Turn turn_of(Run& run) {
    Turn made;
    const std::lock_guard<std::mutex> kept{run.writing};

    made.before = std::exchange(run.written, made.mine);

    return made;
}

// The end of a turn, told when it goes out of scope however it goes: written, failed or cancelled,
// so that the writes after it never wait forever.
struct Ending {
    std::shared_ptr<Pending<void>> mine;

    // Tells the end.
    ~Ending() { mine->settle(); }
};

// Writes `bytes` to `run`'s process in its place `turn`, after the write before it. Throws
// `Trouble` broken when the process cannot be written to.
Task<void> sent_in(std::shared_ptr<Run> run, Turn turn, Bytes bytes) {
    const Ending ending{turn.mine};

    if (turn.before) co_await turn.before->task();

    std::optional<Trouble> refused;

    try {
        co_await run->input.write(std::move(bytes));
    } catch (const Trouble& why) {
        refused = why;
    }

    if (refused) throw ended(run->name, refused->what());
}

// Writes `bytes` to `run`'s process after every write asked before it. Throws `Trouble` broken
// when the process cannot be written to.
Task<void> sent(std::shared_ptr<Run> run, Bytes bytes) {
    Turn turn = turn_of(*run);

    co_await sent_in(std::move(run), std::move(turn), std::move(bytes));
}

// Writes `bytes` to `run`'s process in its place `turn` and lets every trouble go: a process that
// cannot hear anymore is ending, and its reader says so.
Task<void> quietly(std::shared_ptr<Run> run, Turn turn, Bytes bytes) {
    try {
        co_await sent_in(std::move(run), std::move(turn), std::move(bytes));
    } catch (const Trouble&) {
    }
}

// Answers the question `id` of `run`'s process for `much` bytes at `at` of `reader`: a block, or
// unread with the trouble of the read (missing for a document the program does not know).
Task<void> served(std::shared_ptr<Run> run, std::uint32_t id, std::shared_ptr<meta::Reader> reader, std::uint64_t at, std::uint32_t much) {
    Bytes answer;
    Sort sort = Sort::block;

    try {
        if (!reader) throw Trouble{Kind::missing, "No such document."};

        answer = co_await reader->read(at, much);
    } catch (const Trouble& why) {
        sort = Sort::unread;
        answer = rendering::failed_with(why);
    }

    Turn turn = turn_of(*run);

    co_await quietly(std::move(run), std::move(turn), framed_as(sort, id, std::move(answer)));
}

class Engine : public std::enable_shared_from_this<Engine> {
public:
    // An engine on `program`, started through `processes`, held to `limits`; its work runs on the
    // pool of io.
    Engine(Command program, Limits limits, Port<Processes> processes)
        : program_(std::move(program)), limits_(limits), processes_(std::move(processes)), name_(program_.program.name()),
          scope_(std::make_unique<Scope>(io())) {}

    // Kills the process, if one runs; the work around it is cancelled with the scope.
    ~Engine() {
        if (process_.held()) process_.kill();
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // The name of the engine's program.
    [[nodiscard]] const std::string& name() const { return name_; }

    // The limits of time.
    [[nodiscard]] const Limits& limits() const { return limits_; }

    // The number of the process now running, or of the last.
    [[nodiscard]] std::uint64_t generation() const {
        const std::lock_guard<std::mutex> kept{guard_};

        return run_ ? run_->generation : 0;
    }

    // The hello of the running process, the process started first when none runs, within the limit
    // of greeting (on the caller's clock). Throws `Trouble` unsupported when the engine is not
    // there: the process could not start, or ended, kept silent or spoke another version before its
    // hello, now or before.
    Task<rendering::Hello> greeted() {
        const std::pair<std::shared_ptr<Run>, std::shared_ptr<Shared<rendering::Hello>>> greeting = greeting_now();
        bool late = false;
        rendering::Hello made;

        try {
            made = co_await within(limits_.greeting, greeting.second->get());
        } catch (const Overdue&) {
            late = true;
        }

        if (late) {
            const Trouble silent(Kind::unsupported, "The renderer " + name_ + " did not greet in time.");

            lose(greeting.first, silent);

            throw silent;
        }

        co_return made;
    }

    // The answer of the process to the question of `sort` with `payload`, within `limit`: of the
    // process `generation`, which must still run, or of any (zero), started when none runs.
    // Throws `Trouble`: what `greeted` throws; broken when the process `generation` has ended,
    // and what was open in it with it; the trouble the process answered with; overdue for no
    // answer within the limit (the process is killed); broken when the process ends meanwhile;
    // malformed when it breaks the protocol.
    Task<Frame> asked(Sort sort, Bytes payload, std::chrono::milliseconds limit, std::uint64_t generation = 0) {
        if (generation == 0) (void)co_await greeted();

        Pending<Frame> answer;
        const std::pair<std::shared_ptr<Run>, std::uint32_t> asking = waiting_for(answer, generation);
        const std::shared_ptr<Run>& run = asking.first;
        const std::uint32_t id = asking.second;

        Frame got;

        try {
            co_await sent(run, framed_as(sort, id, std::move(payload)));

            got = co_await within(limit, answer.task());
        } catch (const Overdue&) {
            const Trouble late(Kind::overdue, "The renderer " + name_ + " did not answer in time.");

            forget(run, id);
            killed(run, late);

            throw late;
        } catch (...) {
            forget(run, id);

            throw;
        }

        if (got.sort == Sort::failed) throw believed(asking.first->generation, [&got] { return rendering::failure(got.payload); });

        co_return got;
    }

    // What `parse` makes of words of the process `generation`; a process whose words break the
    // protocol is killed. Throws what `parse` throws.
    template <typename Parse>
    auto believed(std::uint64_t generation, Parse parse) -> decltype(parse()) {
        try {
            return parse();
        } catch (const Trouble& why) {
            broke(generation, why);

            throw;
        }
    }

    // Tells the running process of `generation` the question of `sort` with `payload` and waits
    // for nothing: a document closed, a view dismissed; its answer is taken and let go. Its place
    // among the writes is taken now, so it goes before any question asked after it. Nothing when
    // that process is gone.
    void told(std::uint64_t generation, Sort sort, Bytes payload) {
        std::shared_ptr<Run> run;
        std::uint32_t id = 0;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            if (!run_ || run_->ended || run_->generation != generation) return;

            run = run_;
            id = ++questions_;
            run->waiting.emplace(id, Pending<Frame>());
        }

        Turn turn = turn_of(*run);

        scope_->run(quietly(run, std::move(turn), framed_as(sort, id, std::move(payload))));
    }

    // The process `generation` broke the protocol with `why`: killed, and whatever waits on it
    // failed. Nothing when another process runs now.
    void broke(std::uint64_t generation, const Trouble& why) {
        std::shared_ptr<Run> run;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            if (!run_ || run_->generation != generation) return;

            run = run_;
        }

        killed(run, why);
    }

    // A number for the document read through `from`, whose reads the process may ask for.
    std::uint32_t kept(std::shared_ptr<meta::Reader> from) {
        const std::lock_guard<std::mutex> kept{guard_};

        readers_[++numbers_] = std::move(from);

        return numbers_;
    }

    // A number for a view, which reads nothing.
    std::uint32_t numbered() {
        const std::lock_guard<std::mutex> kept{guard_};

        return ++numbers_;
    }

    // Lets go of the reader of the document `number`.
    void forgot(std::uint32_t number) {
        const std::lock_guard<std::mutex> kept{guard_};

        readers_.erase(number);
    }

private:
    // The running process and its greeting, or a process started now, on the pool of io.
    std::pair<std::shared_ptr<Run>, std::shared_ptr<Shared<rendering::Hello>>> greeting_now() {
        const std::lock_guard<std::mutex> kept{guard_};

        if (lost_) throw Trouble{Kind::unsupported, "The renderer " + name_ + " is not there."};

        if (!run_ || run_->ended) {
            auto run = std::make_shared<Run>();

            run->name = name_;
            run->generation = ++generations_;
            run->greeting = std::make_shared<Shared<rendering::Hello>>(begun(weak_from_this(), run), io());
            run_ = run;
        }

        return {run_, run_->greeting};
    }

    // A number for a question to the running process, of `generation` unless that is zero,
    // whose answer goes to `answer`. Throws `Trouble` broken when that process has ended.
    std::pair<std::shared_ptr<Run>, std::uint32_t> waiting_for(const Pending<Frame>& answer, std::uint64_t generation) {
        const std::lock_guard<std::mutex> kept{guard_};

        if (!run_ || run_->ended || (generation != 0 && run_->generation != generation))
            throw ended(name_, generation != 0 ? "what was open in it was lost." : "before it was asked.");

        run_->waiting.emplace(++questions_, answer);

        return {run_, questions_};
    }

    // Forgets the question `id` to `run`.
    void forget(const std::shared_ptr<Run>& run, std::uint32_t id) {
        const std::lock_guard<std::mutex> kept{guard_};

        run->waiting.erase(id);
    }

    // Starts the process of `run`, with the tasks that read its output and errors, and waits for
    // its hello (`greeted` holds the limit); the engine is held only between the waits, so that
    // one let go meanwhile kills what it started. Throws `Trouble` unsupported, the engine then
    // lost for good, when it cannot start, ends first, or greets in another version.
    static Task<rendering::Hello> begun(std::weak_ptr<Engine> weak, std::shared_ptr<Run> run) {
        std::shared_ptr<Engine> engine = weak.lock();

        if (!engine) throw Trouble{Kind::unsupported, "The renderer is gone."};

        const std::string name = engine->name_;
        const Port<Processes> processes = engine->processes_;
        const Command program = engine->program_;
        std::optional<Trouble> refused;

        engine.reset();

        Process started;

        try {
            started = co_await processes->start(program);
        } catch (const Trouble& why) {
            refused = why;
        }

        engine = weak.lock();

        if (!engine) {
            if (started.held()) started.kill();

            throw Trouble{Kind::unsupported, "The renderer is gone."};
        }

        if (refused) {
            engine->lose(run, *refused);

            throw Trouble{Kind::unsupported, "The renderer " + name + " cannot start: " + refused->what()};
        }

        engine->adopted(run, std::move(started));
        engine->scope_->run(listened(weak, run));
        engine->scope_->run(drained_errors(run));
        engine.reset();

        rendering::Hello hello;

        try {
            hello = co_await run->hello.task();
        } catch (const Trouble& why) {
            refused = why;
        }

        engine = weak.lock();

        if (!engine) throw Trouble{Kind::unsupported, "The renderer is gone."};

        if (!refused && hello.version != rendering::version) refused.emplace(Kind::unsupported, "it speaks version " + std::to_string(hello.version) + ".");

        if (refused) {
            engine->lose(run, *refused);

            throw Trouble{Kind::unsupported, "The renderer " + name + " did not greet: " + refused->what()};
        }

        co_return hello;
    }

    // Reads the frames of `run`'s process to its end and hands them on; then the process has
    // ended for every question still waiting.
    static Task<void> listened(std::weak_ptr<Engine> weak, std::shared_ptr<Run> run) {
        rendering::Unframing frames;
        std::optional<Trouble> why;

        try {
            for (;;) {
                const Bytes got = co_await run->output.read(read_at_once);

                if (got.empty()) break;

                frames.add(got);

                for (std::optional<Frame> one = frames.next(); one; one = frames.next()) {
                    std::shared_ptr<Engine> engine = weak.lock();

                    if (!engine) co_return;

                    engine->heard(run, std::move(*one));
                }
            }
        } catch (const Trouble& trouble) {
            why = trouble;
        }

        std::shared_ptr<Engine> engine = weak.lock();

        if (!engine) co_return;

        if (why && why->kind() == Kind::malformed) {
            engine->killed(run, *why);
        } else {
            engine->killed(run, ended(engine->name_, why ? why->what() : "its output closed."));
        }
    }

    // Reads the errors of `run`'s process to their end and lets them go, so that a process that
    // writes there never stops on a full pipe.
    static Task<void> drained_errors(std::shared_ptr<Run> run) {
        try {
            for (;;) {
                const Bytes got = co_await run->errors.read(read_at_once);

                if (got.empty()) break;
            }
        } catch (const Trouble&) {
        }
    }

    // A frame from `run`'s process: its hello, an answer, or a question for bytes of a document.
    // Throws `Trouble` malformed for anything else, a second hello, a frame before the hello or
    // an answer to no question.
    void heard(const std::shared_ptr<Run>& run, Frame frame) {
        if (!run->greeted) {
            if (frame.sort != Sort::hello) throw Trouble{Kind::malformed, "The renderer broke the protocol: no hello first."};

            run->greeted = true;
            run->hello.settle(rendering::greeting(frame.payload));

            return;
        }

        if (frame.sort == Sort::done || frame.sort == Sort::failed) {
            std::optional<Pending<Frame>> answer;

            {
                const std::lock_guard<std::mutex> kept{guard_};
                const auto found = run->waiting.find(frame.id);

                if (found != run->waiting.end()) {
                    answer = found->second;
                    run->waiting.erase(found);
                }
            }

            if (!answer) throw Trouble{Kind::malformed, "The renderer broke the protocol: an answer to no question."};

            answer->settle(std::move(frame));

            return;
        }

        if (frame.sort == Sort::read) {
            rendering::Reading from{frame.payload};
            const std::uint32_t document = from.u32();
            const std::uint64_t at = from.u64();
            const std::uint32_t much = from.u32();

            from.ended();

            if (much > rendering::widest_block) throw Trouble{Kind::malformed, "The renderer broke the protocol: a read of more than a block."};

            std::shared_ptr<meta::Reader> reader;

            {
                const std::lock_guard<std::mutex> kept{guard_};
                const auto found = readers_.find(document);

                if (found != readers_.end()) reader = found->second;
            }

            scope_->run(served(run, frame.id, std::move(reader), at, much));

            return;
        }

        throw Trouble{Kind::malformed, "The renderer broke the protocol: a frame of an unknown sort."};
    }

    // The process `started` is `run`'s: its streams go to the run, the process to the engine.
    // Killed at once when another run has come meanwhile or the run has ended.
    void adopted(const std::shared_ptr<Run>& run, Process started) {
        std::optional<Process> stray;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            run->input = started.input();
            run->output = started.output();
            run->errors = started.errors();

            if (run_ == run && !run->ended) {
                process_ = std::move(started);
            } else {
                stray = std::move(started);
            }
        }

        if (stray) stray->kill();
    }

    // The process of `run` ended before its hello, or greeted wrongly: killed, and the engine
    // lost for good.
    void lose(const std::shared_ptr<Run>& run, const Trouble& why) {
        {
            const std::lock_guard<std::mutex> kept{guard_};

            lost_ = true;
        }

        killed(run, why);
    }

    // The process of `run` ends with `why`: killed when it still runs, its hello and every
    // question waiting failed with `why`.
    void killed(const std::shared_ptr<Run>& run, const Trouble& why) {
        std::map<std::uint32_t, Pending<Frame>> waiting;
        Process process;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            if (run->ended) return;

            run->ended = true;
            waiting.swap(run->waiting);

            if (run_ == run) process = process_;
        }

        if (process.held()) process.kill();

        const std::exception_ptr failed = std::make_exception_ptr(why);

        run->hello.fail(failed);

        for (auto& [id, answer] : waiting) answer.fail(failed);
    }

    Command program_;
    Limits limits_;
    Port<Processes> processes_;
    std::string name_;
    mutable std::mutex guard_;
    std::unique_ptr<Scope> scope_;
    std::shared_ptr<Run> run_;
    Process process_;
    std::uint64_t generations_ = 0;
    std::uint32_t questions_ = 0;
    std::uint32_t numbers_ = 0;
    std::map<std::uint32_t, std::shared_ptr<meta::Reader>> readers_;
    bool lost_ = false;
};

// A document open in a process.
class Opened final : public Document {
public:
    // The document `number` of the process `generation` of `engine`, with `pages`.
    Opened(std::shared_ptr<Engine> engine, std::uint32_t number, std::uint64_t generation, std::vector<Page> pages)
        : engine_(std::move(engine)), number_(number), generation_(generation), pages_(std::move(pages)) {}

    // Closes the document in its process and lets go of its reader.
    ~Opened() override {
        engine_->told(generation_, Sort::close, rendering::Writing().u32(number_).done());
        engine_->forgot(number_);
    }

    // The pages and their sizes.
    [[nodiscard]] const std::vector<Page>& pages() const override { return pages_; }

    // The page `page` at `size`, the part `region` of it. Throws `Trouble`: other for a page that
    // is not there, a size of no pixels or over the ceiling, or a region not within the page;
    // what `Engine::asked` throws, broken among it when the process that opened the document has
    // ended.
    Task<media::Pixels> rendered(std::size_t page, media::Size size, Region region) override {
        if (page >= pages_.size()) throw Trouble{Kind::other, "No such page."};

        sized(size);
        within_page(region);

        Bytes payload = rendering::Writing()
                            .u32(number_)
                            .u32(static_cast<std::uint32_t>(page))
                            .u32(size.width)
                            .u32(size.height)
                            .f64(region.left)
                            .f64(region.top)
                            .f64(region.width)
                            .f64(region.height)
                            .done();
        const Frame got = co_await engine_->asked(Sort::render, std::move(payload), engine_->limits().answer, generation_);

        co_return engine_->believed(generation_, [&got, size] {
            rendering::Reading from{got.payload};

            return rendering::pictured(from, size);
        });
    }

private:
    // Throws `Trouble` (other) for a region not within the page or of no area.
    static void within_page(const Region& region) {
        const bool fits = region.left >= 0 && region.top >= 0 && region.width > 0 && region.height > 0 && region.left + region.width <= 1 &&
                          region.top + region.height <= 1;

        if (!fits) throw Trouble{Kind::other, "A region not within the page."};
    }

    std::shared_ptr<Engine> engine_;
    std::uint32_t number_;
    std::uint64_t generation_;
    std::vector<Page> pages_;
};

// The adapter of `Renderer` on a program that speaks the protocol.
class Rendering final : public Renderer {
public:
    // The program `program`, which takes what `takes` recognizes, held to `limits`.
    Rendering(Command program, Recognizing takes, Limits limits, Port<Processes> processes)
        : engine_(std::make_shared<Engine>(std::move(program), limits, std::move(processes))), takes_(std::move(takes)) {}

    // The name of the program.
    [[nodiscard]] std::string_view name() const override { return engine_->name(); }

    // Whether the program takes a document that starts with `head`.
    [[nodiscard]] bool takes(std::span<const std::byte> head) const override { return takes_ && takes_(head); }

    // The document read through `from`, opened in the process. Throws `Trouble`: unsupported when
    // the engine is not there or renders no documents; what reading the size and `Engine::asked`
    // throw.
    Task<std::shared_ptr<Document>> opened(std::shared_ptr<meta::Reader> from) override {
        const std::uint64_t size = co_await from->size();
        const rendering::Hello hello = co_await engine_->greeted();

        if ((hello.abilities & rendering::documents) == 0) throw Trouble{Kind::unsupported, "The renderer " + engine_->name() + " renders no documents."};

        const std::uint64_t generation = engine_->generation();
        const std::uint32_t number = engine_->kept(std::move(from));
        std::vector<Page> pages;

        try {
            const Frame got = co_await engine_->asked(Sort::open, rendering::Writing().u32(number).u64(size).done(), engine_->limits().answer, generation);

            pages = engine_->believed(generation, [&got] { return rendering::paged(got.payload); });
        } catch (...) {
            engine_->forgot(number);

            throw;
        }

        co_return std::make_shared<Opened>(engine_, number, generation, std::move(pages));
    }

private:
    std::shared_ptr<Engine> engine_;
    Recognizing takes_;
};

// A view open in a process.
class Viewed final : public Browsed {
public:
    // The view `number` of the process `generation` of `engine`, at `size`.
    Viewed(std::shared_ptr<Engine> engine, std::uint32_t number, std::uint64_t generation, media::Size size)
        : engine_(std::move(engine)), number_(number), generation_(generation), size_(size) {}

    // Dismisses the view in its process.
    ~Viewed() override { engine_->told(generation_, Sort::dismiss, rendering::Writing().u32(number_).done()); }

    // The picture of the view, its title and address. Throws what `Engine::asked` throws, broken
    // among it when the process of the view has ended.
    Task<Shot> shot() override {
        const media::Size size = current();
        const Frame got = co_await engine_->asked(Sort::shot, rendering::Writing().u32(number_).done(), engine_->limits().answer, generation_);

        co_return engine_->believed(generation_, [&got, size] {
            rendering::Reading from{got.payload};
            Shot made;

            made.title = from.text();
            made.address = from.text();
            made.pixels = rendering::pictured(from, size);

            return made;
        });
    }

    // The view brought to `size`. Throws `Trouble`: other for a size of no pixels or over the
    // ceiling; what `Engine::asked` throws, broken among it when the process of the view has
    // ended.
    Task<void> resized(media::Size size) override {
        sized(size);
        (void)co_await engine_->asked(Sort::resize, rendering::Writing().u32(number_).u32(size.width).u32(size.height).done(), engine_->limits().answer,
                                      generation_);

        const std::lock_guard<std::mutex> kept{guard_};

        size_ = size;
    }

private:
    // The size the view has now.
    media::Size current() const {
        const std::lock_guard<std::mutex> kept{guard_};

        return size_;
    }

    std::shared_ptr<Engine> engine_;
    std::uint32_t number_;
    std::uint64_t generation_;
    mutable std::mutex guard_;
    media::Size size_;
};

// The adapter of `Browser` on a program that speaks the protocol.
class Showing final : public Browser {
public:
    // The program `program`, held to `limits`.
    Showing(Command program, Limits limits, Port<Processes> processes) : engine_(std::make_shared<Engine>(std::move(program), limits, std::move(processes))) {}

    // The name of the program.
    [[nodiscard]] std::string_view name() const override { return engine_->name(); }

    // The page at `address` in a view of `size`, with scripts and the network as `how` allows.
    // Throws `Trouble`: other for a size of no pixels or over the ceiling; unsupported when the
    // engine is not there, shows no pages, or cannot allow what `how` asks for; what
    // `Engine::asked` throws.
    Task<std::shared_ptr<Browsed>> opened(std::string address, Browsing how, media::Size size) override {
        sized(size);

        const rendering::Hello hello = co_await engine_->greeted();

        if ((hello.abilities & rendering::pages) == 0) throw Trouble{Kind::unsupported, "The renderer " + engine_->name() + " shows no pages."};

        if (how.scripts && (hello.abilities & rendering::scripts) == 0) throw Trouble{Kind::unsupported, "The renderer " + engine_->name() + " runs no scripts."};

        if (how.network && (hello.abilities & rendering::network) == 0) throw Trouble{Kind::unsupported, "The renderer " + engine_->name() + " has no network."};

        const std::uint64_t generation = engine_->generation();
        const std::uint32_t number = engine_->numbered();
        Bytes payload = rendering::Writing()
                            .u32(number)
                            .text(address)
                            .u8(how.scripts ? 1 : 0)
                            .u8(how.network ? 1 : 0)
                            .u32(size.width)
                            .u32(size.height)
                            .done();

        (void)co_await engine_->asked(Sort::navigate, std::move(payload), engine_->limits().browsing, generation);

        co_return std::make_shared<Viewed>(engine_, number, generation, size);
    }

private:
    std::shared_ptr<Engine> engine_;
};

// A document of the scripted renderer: its pages, each rendered in one tone, the number of the
// page.
class Played final : public Document {
public:
    // The pages `pages`.
    explicit Played(std::vector<Page> pages) : pages_(std::move(pages)) {}

    // The pages.
    [[nodiscard]] const std::vector<Page>& pages() const override { return pages_; }

    // The page `page` at `size`, opaque in the tone of its number. Throws `Trouble` other for a
    // page that is not there or a size of no pixels or over the ceiling.
    Task<media::Pixels> rendered(std::size_t page, media::Size size, Region) override {
        if (page >= pages_.size()) throw Trouble{Kind::other, "No such page."};

        sized(size);
        co_return toned(size, static_cast<std::uint8_t>(page));
    }

    // A picture of `size` in the tone `tone`, opaque.
    static media::Pixels toned(media::Size size, std::uint8_t tone) {
        std::vector<media::Still> stills(1);

        for (std::uint64_t at = 0; at < std::uint64_t{size.width} * size.height; ++at)
            for (const std::uint8_t band : {tone, tone, tone, std::uint8_t{255}}) stills.front().rgba.push_back(std::byte{band});

        return media::Pixels{size, std::move(stills)};
    }

private:
    std::vector<Page> pages_;
};

// A view of the scripted browser: its address as its title, in white.
class Shown final : public Browsed {
public:
    // The view of `address` at `size`.
    Shown(std::string address, media::Size size) : address_(std::move(address)), size_(size) {}

    // The view in white, titled with its address.
    Task<Shot> shot() override {
        Shot made;

        made.pixels = Played::toned(size_, 255);
        made.title = address_;
        made.address = address_;

        co_return made;
    }

    // The view at `size`. Throws `Trouble` other for a size of no pixels or over the ceiling.
    Task<void> resized(media::Size size) override {
        sized(size);
        size_ = size;
        co_return;
    }

private:
    std::string address_;
    media::Size size_;
};

}

// A chain of the renderers `links`, asked in their order, under `name`. Throws `Trouble` (other)
// for a link of no renderer.
Renderers::Renderers(std::vector<std::shared_ptr<Renderer>> links, std::string name) : links_(std::move(links)), name_(std::move(name)) {
    for (const std::shared_ptr<Renderer>& one : links_)
        if (one == nullptr) throw Trouble{Kind::other, "A link of no renderer."};
}

// Whether a link takes the document that starts with `head`.
bool Renderers::takes(std::span<const std::byte> head) const {
    for (const std::shared_ptr<Renderer>& one : links_)
        if (one->takes(head)) return true;

    return false;
}

// The document read through `from`, opened by the first link that takes its head. Throws
// `Trouble`: unsupported when none does; what that link throws.
Task<std::shared_ptr<Document>> Renderers::opened(std::shared_ptr<meta::Reader> from) {
    const std::uint64_t size = co_await from->size();
    const Bytes head = co_await from->read(0, static_cast<std::size_t>(std::min<std::uint64_t>(size, 1024)));

    for (const std::shared_ptr<Renderer>& one : links_)
        if (one->takes(head)) co_return co_await one->opened(std::move(from));

    throw Trouble{Kind::unsupported, "No renderer takes the document."};
}

// A chain of the browsers `links`, tried in their order, under `name`. Throws `Trouble` (other)
// for a link of no browser.
Browsers::Browsers(std::vector<std::shared_ptr<Browser>> links, std::string name) : links_(std::move(links)), name_(std::move(name)) {
    for (const std::shared_ptr<Browser>& one : links_)
        if (one == nullptr) throw Trouble{Kind::other, "A link of no browser."};
}

// The page at `address`, opened by the first link that can: a link that refuses (`Trouble`
// unsupported: its engine is not there, or cannot allow what `how` asks for) passes it to the
// next. Throws `Trouble`: unsupported when every link refuses; what a link throws otherwise.
Task<std::shared_ptr<Browsed>> Browsers::opened(std::string address, Browsing how, media::Size size) {
    for (const std::shared_ptr<Browser>& one : links_) {
        std::shared_ptr<Browsed> made;

        try {
            made = co_await one->opened(address, how, size);
        } catch (const Trouble& why) {
            if (why.kind() != Kind::unsupported) throw;
        }

        if (made) co_return made;
    }

    throw Trouble{Kind::unsupported, "No browser can open the page."};
}

// The adapter of `Renderer` on `program`, a program that speaks the protocol of tal's renderers
// (see rendering.cpp), for the documents `takes` recognizes, held to `limits`.
std::shared_ptr<Renderer> renderer(Command program, Recognizing takes, Limits limits, Port<Processes> processes) {
    return std::make_shared<Rendering>(std::move(program), std::move(takes), limits, std::move(processes));
}

// The adapter of `Browser` on `program`, a program that speaks the protocol of tal's renderers,
// held to `limits`.
std::shared_ptr<Browser> browser(Command program, Limits limits, Port<Processes> processes) {
    return std::make_shared<Showing>(std::move(program), limits, std::move(processes));
}

// A scripted renderer whose documents have the pages `given`.
ScriptedRenderer::ScriptedRenderer(std::vector<Page> given) : pages(std::move(given)) {}

// Whether it takes the document: any.
bool ScriptedRenderer::takes(std::span<const std::byte>) const { return true; }

// A document of the scripted pages, whatever `from` holds; counted in `opening`.
Task<std::shared_ptr<Document>> ScriptedRenderer::opened(std::shared_ptr<meta::Reader>) {
    ++opening;
    co_return std::make_shared<Played>(pages);
}

// A view of `address` at `size`, kept in `addresses` and `ways`. Throws `Trouble` other for a
// size of no pixels or over the ceiling.
Task<std::shared_ptr<Browsed>> ScriptedBrowser::opened(std::string address, Browsing how, media::Size size) {
    sized(size);
    addresses.push_back(address);
    ways.push_back(how);
    co_return std::make_shared<Shown>(std::move(address), size);
}

}
