#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "tal/console.hpp"
#include "tal/fs.hpp"
#include "tal/path.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

namespace tal {

enum class Started { opened, unassociated, sessionless, terminal };

enum class Origin { here, internet };

struct Application {
    std::string name;
    std::string id;
    bool preferred = false;
    bool terminal = false;

    friend bool operator==(const Application&, const Application&) = default;
};

struct Opened {
    Started started = Started::opened;
    Command command;
};

class Desktop : public Adapter {
public:
    static constexpr std::string_view kind = "desktop";

    [[nodiscard]] static std::shared_ptr<Desktop> builtin();

    [[nodiscard]] virtual Task<std::optional<Command>> program(Path file) = 0;

    [[nodiscard]] virtual Task<Origin> origin(Path file) = 0;

    [[nodiscard]] virtual Task<Opened> open(Path file) = 0;

    [[nodiscard]] virtual Task<std::vector<Application>> applications(Path file) = 0;

    [[nodiscard]] virtual Task<std::vector<Application>> applications_for(std::string extension) = 0;

    [[nodiscard]] virtual Task<Opened> open_with(Path file, Application with) = 0;

    [[nodiscard]] virtual Task<Opened> open_by(Path file, Path program) = 0;

    [[nodiscard]] virtual Task<Opened> chosen(Path file) = 0;
};

[[nodiscard]] Port<Desktop> desktop(Port<Console> console, Port<Processes> processes, Port<fs::Files> files);

class ScriptedDesktop : public Desktop {
public:
    enum class Way { open, with, by, chosen };

    struct Request {
        Way way = Way::open;
        Path file;
        std::string with;
    };

    std::map<Path, Command> programs;
    std::set<Path> downloaded;
    std::map<std::string, std::vector<Application>, std::less<>> offered;
    Opened answer;
    std::optional<Trouble> refusal;
    std::vector<Request> requests;

    [[nodiscard]] std::string_view name() const override { return "scripted"; }

    [[nodiscard]] Task<std::optional<Command>> program(Path file) override;

    [[nodiscard]] Task<Origin> origin(Path file) override;

    [[nodiscard]] Task<Opened> open(Path file) override;

    [[nodiscard]] Task<std::vector<Application>> applications(Path file) override;

    [[nodiscard]] Task<std::vector<Application>> applications_for(std::string extension) override;

    [[nodiscard]] Task<Opened> open_with(Path file, Application with) override;

    [[nodiscard]] Task<Opened> open_by(Path file, Path program) override;

    [[nodiscard]] Task<Opened> chosen(Path file) override;
};

}
