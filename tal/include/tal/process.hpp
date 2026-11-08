#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/path.hpp"
#include "tal/port.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"

namespace tal {

struct Variable {
    std::string name;
    std::optional<std::string> value;
};

enum class Mode { piped, terminal, detached, opened };

struct Command {
    Path program;
    std::vector<std::string> arguments;
    std::vector<Variable> environment;
    Path folder;
    Mode mode = Mode::piped;
    bool merged = false;
    int columns = 80;
    int rows = 24;
};

struct Exit {
    enum class Ended { normal, killed, lost };

    Ended ended = Ended::normal;
    int code = 0;
    int signal = 0;

    friend bool operator==(const Exit&, const Exit&) = default;
};

struct Output {
    Exit exit;
    Bytes output;
    Bytes errors;
};

class Spawned {
public:
    Spawned() = default;
    virtual ~Spawned() = default;

    Spawned(const Spawned&) = delete;
    Spawned& operator=(const Spawned&) = delete;

    [[nodiscard]] virtual std::int64_t id() const = 0;

    [[nodiscard]] virtual Task<Exit> ended() = 0;

    virtual void stop() = 0;

    virtual void kill() = 0;

    virtual void resize(int columns, int rows) = 0;
};

class Process {
public:
    Process() = default;

    Process(std::shared_ptr<Spawned> held, Stream input, Stream output, Stream errors);

    [[nodiscard]] static Task<Output> run(Command what, Bytes input = {});

    [[nodiscard]] static Task<Process> start(Command what);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] std::int64_t id() const;

    [[nodiscard]] const Stream& input() const { return input_; }

    [[nodiscard]] const Stream& output() const { return output_; }

    [[nodiscard]] const Stream& errors() const { return errors_; }

    [[nodiscard]] Task<Exit> ended() const;

    void stop() const;

    void kill() const;

    void resize(int columns, int rows) const;

private:
    std::shared_ptr<Spawned> held_;
    Stream input_;
    Stream output_;
    Stream errors_;
};

class Processes : public Adapter {
public:
    static constexpr std::string_view kind = "processes";

    [[nodiscard]] virtual Task<Process> start(Command what) = 0;
};

struct Running {
    Command command;
    Stream input;
    Stream output;
    Stream errors;
    std::atomic<int> columns{80};
    std::atomic<int> rows{24};
};

class Scripted : public Processes {
public:
    explicit Scripted(Executor& where = loop());

    ~Scripted() override;

    Scripted(const Scripted&) = delete;
    Scripted& operator=(const Scripted&) = delete;

    [[nodiscard]] std::string_view name() const override { return "scripted"; }

    template <typename F>
        requires std::is_invocable_r_v<Task<int>, F&, Running&>
    void add(std::string program, F script) {
        struct Held : Script {
            F script;

            explicit Held(F given) : script(std::move(given)) {}

            Task<int> run(Running& with) override { return script(with); }
        };

        added(std::move(program), std::make_shared<Held>(std::move(script)));
    }

    [[nodiscard]] std::vector<Command> started() const;

    [[nodiscard]] Task<Process> start(Command what) override;

private:
    struct Script {
        Script() = default;
        virtual ~Script() = default;

        Script(const Script&) = delete;
        Script& operator=(const Script&) = delete;

        virtual Task<int> run(Running& with) = 0;
    };

    struct State;

    void added(std::string program, std::shared_ptr<Script> script);

    std::shared_ptr<State> state_;
};

}
