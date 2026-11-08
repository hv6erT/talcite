#include "tal/process.hpp"

#include "tal/trouble.hpp"

// Processes of tal::core: the handle a program works with, the free way to run one to its end,
// and the scripted adapter, whose programs are coroutines a trial writes. A scripted program runs
// on the adapter's executor with pipes in memory for its streams; stopping or killing it cancels
// its coroutine, and its streams end either way.

namespace tal {

// A process over what the adapter gave, with its three streams.
Process::Process(std::shared_ptr<Spawned> held, Stream input, Stream output, Stream errors)
    : held_(std::move(held)), input_(std::move(input)), output_(std::move(output)), errors_(std::move(errors)) {}

// Runs a program to its end through the default adapter: writes `input` to it, gathers what it
// writes and how it ended. A program that stops reading early does not make the run fail.
Task<Output> Process::run(Command what, Bytes input) {
    const Process child = co_await start(std::move(what));

    try {
        if (!input.empty()) co_await child.input().write(std::move(input));
    } catch (const Trouble& why) {
        if (why.kind() != Kind::broken) throw;
    }

    child.input().close();

    auto [output, errors] = co_await all(drained(child.output()), drained(child.errors()));
    const Exit exit = co_await child.ended();

    co_return Output{exit, std::move(output), std::move(errors)};
}

// Starts a program through the default adapter.
Task<Process> Process::start(Command what) {
    const Port<Processes> processes;

    co_return co_await processes->start(std::move(what));
}

// The system's number of the process; 0 when there is none.
std::int64_t Process::id() const { return held_ ? held_->id() : 0; }

// How the process ended, once it has; any number of tasks may wait for it.
Task<Exit> Process::ended() const {
    return [](std::shared_ptr<Spawned> held) -> Task<Exit> {
        if (held == nullptr) throw Trouble{Kind::missing, "No process is held."};

        co_return co_await held->ended();
    }(held_);
}

// Asks the process to end, and ends it by force if it does not soon.
void Process::stop() const {
    if (held_) held_->stop();
}

// Ends the process by force.
void Process::kill() const {
    if (held_) held_->kill();
}

// Tells a process in a terminal its new size.
void Process::resize(int columns, int rows) const {
    if (held_) held_->resize(columns, rows);
}

namespace {

// The signals a scripted program is said to end by.
constexpr int stopping_signal = 15;
constexpr int killing_signal = 9;

}

// The scripts, the programs started and the work of those still running.
struct Scripted::State {
    std::mutex guard;
    Executor* where = nullptr;
    std::map<std::string, std::shared_ptr<Script>, std::less<>> scripts;
    std::vector<Command> started;
    std::vector<std::shared_ptr<Scope>> running;
    std::vector<std::weak_ptr<Spawned>> acting;
    std::int64_t numbered = 1000;
};

namespace {

// A scripted program while it runs, and how it ended once it has.
struct Acting : Spawned {
    std::int64_t number = 0;
    std::shared_ptr<Running> running;
    std::shared_ptr<Scope> scope;
    Pending<Exit> exit;
    Shared<Exit> exited;

    // A program with the given number, its streams and the scope its coroutine runs in.
    Acting(std::int64_t given, std::shared_ptr<Running> streams, std::shared_ptr<Scope> held, Executor& where)
        : number(given), running(std::move(streams)), scope(std::move(held)), exited(exit.task(), where) {}

    // The number of the program.
    [[nodiscard]] std::int64_t id() const override { return number; }

    // How it ended.
    Task<Exit> ended() override { return exited.get(); }

    // A scripted program stops at once.
    void stop() override { ended_by(stopping_signal); }

    // Ends the program by force.
    void kill() override { ended_by(killing_signal); }

    // The program sees the new size of its terminal.
    void resize(int columns, int rows) override {
        if (running == nullptr) return;

        running->columns = columns;
        running->rows = rows;
    }

    // Cancels the coroutine, ends its streams and settles the exit as killed by `signal`.
    void ended_by(int signal) {
        if (scope) scope->cancel();

        exit.settle(Exit{Exit::Ended::killed, 0, signal});

        if (running == nullptr) return;

        running->output.close();
        running->errors.close();
        running->input.close();
    }
};

// Runs a script to its end: its code, or lost when it fails; its streams end with it.
Task<void> acted(std::shared_ptr<Running> running, Task<int> script, Pending<Exit> exit) {
    Exit made;

    try {
        made.code = co_await std::move(script);
    } catch (const Cancelled&) {
        throw;
    } catch (...) {
        made.ended = Exit::Ended::lost;
    }

    running->output.close();
    running->errors.close();
    running->input.close();
    exit.settle(made);
}

// Where the streams of a detached program go: what it writes is dropped, and it reads nothing.
struct Sink : Channel {
    // Nothing to read.
    Task<Bytes> read(std::size_t) override {
        return []() -> Task<Bytes> { co_return Bytes{}; }();
    }

    // The bytes are dropped.
    Task<void> write(Bytes) override {
        return []() -> Task<void> { co_return; }();
    }

    // Nothing to end.
    void close() override {}
};

// The reading end of a stream that has already ended.
Stream ended_stream() { return detail::piped().first; }

// The writing end of a stream nobody reads.
Stream deaf_stream() {
    auto [reading, writing] = detail::piped();

    reading.close();

    return writing;
}

}

// A scripted adapter whose programs run on `where`.
Scripted::Scripted(Executor& where) : state_(std::make_shared<State>()) { state_->where = &where; }

// Ends every program still running: one whose process is still held ends as killed, with its
// streams ended.
Scripted::~Scripted() {
    std::vector<std::shared_ptr<Scope>> all;
    std::vector<std::weak_ptr<Spawned>> held;

    {
        const std::lock_guard<std::mutex> kept{state_->guard};

        all.swap(state_->running);
        held.swap(state_->acting);
    }

    for (const auto& weak : held)
        if (const auto one = weak.lock()) one->kill();

    for (const auto& one : all) one->cancel();
}

// Keeps a script for a program, named by its path or by its name alone.
void Scripted::added(std::string program, std::shared_ptr<Script> script) {
    const std::lock_guard<std::mutex> kept{state_->guard};

    state_->scripts[std::move(program)] = std::move(script);
}

// The commands started so far, oldest first.
std::vector<Command> Scripted::started() const {
    const std::lock_guard<std::mutex> kept{state_->guard};

    return state_->started;
}

// Starts the script of the program: piped and in a terminal with its streams, detached on its own
// with none, and opened without any script at all.
Task<Process> Scripted::start(Command what) {
    return [](std::shared_ptr<State> state, Command command) -> Task<Process> {
        std::shared_ptr<Script> script;
        std::int64_t number = 0;

        {
            const std::lock_guard<std::mutex> kept{state->guard};

            state->started.push_back(command);
            std::erase_if(state->running, [](const std::shared_ptr<Scope>& one) { return !one->busy(); });
            std::erase_if(state->acting, [](const std::weak_ptr<Spawned>& one) { return one.expired(); });

            if (command.mode != Mode::opened) {
                auto found = state->scripts.find(command.program.stored());

                if (found == state->scripts.end()) found = state->scripts.find(command.program.name());

                if (found == state->scripts.end())
                    throw Trouble{Kind::missing, "There is no program " + command.program.text() + ".", command.program.text()};

                script = found->second;
                number = ++state->numbered;
            }
        }

        if (command.mode == Mode::opened) {
            const auto acting = std::make_shared<Acting>(0, nullptr, nullptr, *state->where);

            acting->exit.settle(Exit{Exit::Ended::lost, 0, 0});

            co_return Process{acting, deaf_stream(), ended_stream(), ended_stream()};
        }

        auto [script_input, our_input] = detail::piped();
        auto [our_output, script_output] = detail::piped();
        auto [our_errors, script_errors] = detail::piped();
        const auto running = std::make_shared<Running>();
        const bool detached = command.mode == Mode::detached;

        running->command = command;
        running->input = detached ? ended_stream() : script_input;
        running->output = detached ? Stream{std::make_shared<Sink>()} : script_output;
        running->errors = detached ? running->output : command.merged ? script_output : script_errors;
        running->columns = command.columns;
        running->rows = command.rows;

        const auto scope = std::make_shared<Scope>(*state->where);
        const auto acting = std::make_shared<Acting>(number, running, scope, *state->where);

        {
            const std::lock_guard<std::mutex> kept{state->guard};

            state->running.push_back(scope);
            state->acting.push_back(acting);
        }

        scope->run(acted(running, script->run(*running), acting->exit));

        if (detached) {
            acting->exit.settle(Exit{Exit::Ended::lost, 0, 0});

            co_return Process{acting, deaf_stream(), ended_stream(), ended_stream()};
        }

        co_return Process{acting, our_input, our_output, command.merged ? ended_stream() : our_errors};
    }(state_, std::move(what));
}

}
