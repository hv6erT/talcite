#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/clock.hpp"
#include "tal/executor.hpp"
#include "tal/notify.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "notifying.hpp"
#include "quieting.hpp"
#include "tal/testing.hpp"

// Notifications of the system, on scripted programs: gdbus, then notify-send, on freedesktop, and
// osascript on macOS, both ways on every system, so that no trial shows a real notification. The
// texts reach the programs as literals of their languages, and nothing a text holds ends them.

namespace {

using namespace std::chrono_literals;

// The programs of a desktop: which are installed, the status they end with, whether they hang and
// whether one that hung was killed.
struct Desktop {
    std::set<std::string> present;
    int status = 0;
    bool stuck = false;
    bool killed = false;
    tal::Pending<int> never;
};

// Marks the desktop's program killed when the program, hanging, goes away.
struct Hanging {
    Desktop& desktop;

    // Marks it.
    ~Hanging() { desktop.killed = true; }
};

// A program of notifications as `desktop` plays it: ends with its status, or hangs until killed.
tal::Task<int> played(Desktop& desktop) {
    if (!desktop.stuck) co_return desktop.status;

    const Hanging hanging{desktop};

    co_return co_await desktop.never.task();
}

// A notifier on scripted programs, on a loop with a manual clock.
struct Bench {
    std::shared_ptr<tal::ManualClock> clock = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{clock}};
    std::shared_ptr<tal::Scripted> processes = std::make_shared<tal::Scripted>(loop);
    Desktop desktop;
    tal::Port<tal::Notifier> notifier;

    // A desktop of macOS when `apple`, of freedesktop otherwise, with `programs` installed.
    Bench(bool apple, std::set<std::string> programs)
        : notifier(tal::notifying::programs(tal::Port<tal::Processes>{processes}, apple)) {
        desktop.present = std::move(programs);

        for (const char* each : {"gdbus", "notify-send", "osascript"}) {
            if (!desktop.present.contains(each)) continue;

            processes->add(each, [this](tal::Running&) { return played(desktop); });
        }
    }

    // Tells `notice`; the kind of trouble it threw, or none.
    std::optional<tal::Kind> told(tal::Notice notice) {
        try {
            tal::wait(loop, notifier->tell(std::move(notice)));
        } catch (const tal::Trouble& trouble) {
            return trouble.kind();
        }

        return std::nullopt;
    }

    // The arguments of each program started, its name first.
    std::vector<std::vector<std::string>> started() const {
        std::vector<std::vector<std::string>> made;

        for (const tal::Command& each : processes->started()) {
            made.push_back({each.program.text()});
            made.back().insert(made.back().end(), each.arguments.begin(), each.arguments.end());
        }

        return made;
    }
};

// What gdbus is given for a notice with `title`, `body` and the urgency `level`.
std::vector<std::string> gdbus(std::string title, std::string body, int level) {
    return {"gdbus", "call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path", "/org/freedesktop/Notifications",
            "--method", "org.freedesktop.Notifications.Notify", "''", "0", "''", std::move(title), std::move(body), "[]",
            "{'urgency': <byte " + std::to_string(level) + ">}", "int32 -1"};
}

}

// GVariant's literal: in single quotes, a quote and a backslash after a backslash, every control
// character and DEL as \uXXXX, the rest (UTF-8 too) as it is.
TRIAL("Notifying: texts as GVariant literals") {
    SAME(tal::notifying::gvariant(""), std::string{"''"});
    SAME(tal::notifying::gvariant("Poirot"), std::string{"'Poirot'"});
    SAME(tal::notifying::gvariant("l'Orient"), std::string{"'l\\'Orient'"});
    SAME(tal::notifying::gvariant("a\\b"), std::string{"'a\\\\b'"});
    SAME(tal::notifying::gvariant("one\ntwo\t\x1f\x7f"), std::string{"'one\\u000atwo\\u0009\\u001f\\u007f'"});
    SAME(tal::notifying::gvariant(std::string{"a\0b", 3}), std::string{"'a\\u0000b'"});
    SAME(tal::notifying::gvariant("\" ~ żółw"), std::string{"'\" ~ żółw'"});
}

// AppleScript's literal: in double quotes, a quote and a backslash after a backslash, a line break
// and a tab spelled out, the other control characters and DEL left out.
TRIAL("Notifying: texts as AppleScript literals") {
    SAME(tal::notifying::applescript(""), std::string{"\"\""});
    SAME(tal::notifying::applescript("say \"hi\""), std::string{"\"say \\\"hi\\\"\""});
    SAME(tal::notifying::applescript("a\\b"), std::string{"\"a\\\\b\""});
    SAME(tal::notifying::applescript("one\ntwo\tthree"), std::string{"\"one\\ntwo\\tthree\""});
    SAME(tal::notifying::applescript(std::string{"a\0\r\x1b\x7f b", 7}), std::string{"\"a b\""});
    SAME(tal::notifying::applescript("' ~ żółw"), std::string{"\"' ~ żółw\""});
}

// On freedesktop the service is called through gdbus with the urgency as a hint; when that fails
// or is not there notify-send is asked, the texts after `--`; with neither the notice is
// unsupported.
TRIAL("Notifying: gdbus, then notify-send") {
    Bench both{false, {"gdbus", "notify-send"}};

    OUGHT(!both.told({"Done", "It's \\ done", tal::Urgency::low}).has_value());
    OUGHT(both.started() == (std::vector<std::vector<std::string>>{gdbus("'Done'", "'It\\'s \\\\ done'", 0)}));

    OUGHT(!both.told({"A", "B"}).has_value());
    OUGHT(both.started().back() == gdbus("'A'", "'B'", 1));

    OUGHT(!both.told({"A", "B", tal::Urgency::critical}).has_value());
    OUGHT(both.started().back() == gdbus("'A'", "'B'", 2));

    OUGHT(!both.told({"a\xff", "\xc3"}).has_value());
    OUGHT(both.started().back() == gdbus("'a\xef\xbf\xbd'", "'\xef\xbf\xbd'", 1));

    Bench failing{false, {"gdbus", "notify-send"}};

    failing.desktop.status = 1;

    OUGHT(failing.told({"-t", "--help", tal::Urgency::critical}) == tal::Kind::unsupported);
    OUGHT(failing.started().size() == 2);
    OUGHT(failing.started()[1] == (std::vector<std::string>{"notify-send", "--urgency=critical", "--", "-t", "--help"}));

    Bench sending{false, {"notify-send"}};

    OUGHT(!sending.told({"T", "B", tal::Urgency::low}).has_value());
    OUGHT(sending.started().size() == 2);
    OUGHT(sending.started()[1] == (std::vector<std::string>{"notify-send", "--urgency=low", "--", "T", "B"}));

    Bench normal{false, {"notify-send"}};

    OUGHT(!normal.told({"T", "B"}).has_value());
    OUGHT(normal.started().back()[1] == "--urgency=normal");

    Bench none{false, {"osascript"}};

    OUGHT(none.told({"T", "B"}) == tal::Kind::unsupported);
    OUGHT(none.started().size() == 2);
}

// On macOS one line of AppleScript shows the notice; when osascript fails or is not there, the
// notice is unsupported, and the programs of freedesktop are never asked.
TRIAL("Notifying: osascript on macOS") {
    Bench mac{true, {"osascript", "gdbus"}};

    OUGHT(!mac.told({"Title \"q\"", "Body\nnext", tal::Urgency::critical}).has_value());
    OUGHT(mac.started() == (std::vector<std::vector<std::string>>{
                               {"osascript", "-e", "display notification \"Body\\nnext\" with title \"Title \\\"q\\\"\""}}));

    Bench failing{true, {"osascript", "gdbus", "notify-send"}};

    failing.desktop.status = 1;

    OUGHT(failing.told({"T", "B"}) == tal::Kind::unsupported);
    OUGHT(failing.started().size() == 1);

    Bench none{true, {"gdbus"}};

    OUGHT(none.told({"T", "B"}) == tal::Kind::unsupported);
    OUGHT(none.started().size() == 1);
    OUGHT(none.started()[0][0] == "osascript");
}

// A program that hangs is killed after a second and a half, and the next is asked.
TRIAL("Notifying: a hanging program gives way") {
    Bench bench{false, {"gdbus", "notify-send"}};
    tal::Scope scope{bench.loop};
    std::optional<bool> done;

    bench.desktop.stuck = true;
    scope.run([](Bench& on, std::optional<bool>& into) -> tal::Task<void> {
        tal::Notice notice{"T", "B"};

        try {
            co_await on.notifier->tell(std::move(notice));
            into = true;
        } catch (const tal::Trouble&) {
            into = false;
        }
    }(bench, done));

    QUIETED(bench.loop);

    OUGHT(!done.has_value());
    OUGHT(bench.started().size() == 1);

    bench.clock->advance(1499ms);

    QUIETED(bench.loop);

    OUGHT(!done.has_value());
    OUGHT(!bench.desktop.killed);

    bench.clock->advance(1ms);

    QUIETED(bench.loop);

    OUGHT(bench.desktop.killed);
    OUGHT(bench.started().size() == 2);
    OUGHT(bench.started()[1][0] == "notify-send");

    bench.clock->advance(1500ms);

    UNTIL(bench.loop, done.has_value());

    OUGHT(!*done);
}

// The scripted notifier keeps what it is told; the free `tell` goes through the port given, or the
// default one.
TRIAL("Notifying: the scripted notifier and the free tell") {
    auto scripted = std::make_shared<tal::ScriptedNotifier>();
    tal::Loop loop;

    SAME(scripted->name(), std::string_view{"scripted"});
    SAME(tal::Notifier::kind, std::string_view{"notifier"});

    tal::wait(loop, tal::tell({"One", "1"}, tal::Port<tal::Notifier>{scripted}));

    tal::defaults().set<tal::Notifier>(scripted);
    tal::wait(loop, tal::tell({"Two", "2", tal::Urgency::critical}));
    tal::defaults().clear<tal::Notifier>();

    OUGHT(scripted->told == (std::vector<tal::Notice>{{"One", "1"}, {"Two", "2", tal::Urgency::critical}}));
    SAME(tal::Notifier::builtin()->name(), std::string_view{"system"});
    SAME(tal::notifier(tal::Port<tal::Processes>{std::make_shared<tal::Scripted>()})->name(), std::string_view{"system"});
}

#ifdef _WIN32

// On Windows the balloon is the system's own, so the trial shows two, which the system queues; the
// icons go with the notifier. The builtin one is the same for every call.
TRIAL("Notifying: the balloon of Windows") {
    tal::Loop loop;
    tal::Port<tal::Notifier> balloon = tal::notifier(tal::Port<tal::Processes>{std::make_shared<tal::Scripted>(loop)});

    tal::wait(loop, balloon->tell({"tal-test", "A notice of the trials.", tal::Urgency::low}));
    tal::wait(loop, balloon->tell({"tal-test", "Another one after it: zażółć gęślą jaźń.", tal::Urgency::low}));

    OUGHT(tal::Notifier::builtin() == tal::Notifier::builtin());
}

#endif
