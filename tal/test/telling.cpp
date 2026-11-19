#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tal/app.hpp"
#include "tal/clock.hpp"
#include "tal/console.hpp"
#include "tal/executor.hpp"
#include "tal/notify.hpp"
#include "tal/port.hpp"
#include "tal/task.hpp"

#include "tal/testing.hpp"
#include "telling.hpp"

// Notifications through the terminal: the sequence chosen by the terminal's name, and texts that
// can never end the sequence they ride in.

using tal::telling::Way;

// kitty by its TERM, foot by its own, iTerm2, WezTerm and ghostty by TERM_PROGRAM; any other gets
// none.
TRIAL("Telling: the sequence by the terminal's name") {
    OUGHT(tal::telling::way("xterm-kitty") == Way::kitty);
    OUGHT(tal::telling::way("foot") == Way::notify);
    OUGHT(tal::telling::way("foot-extra") == Way::notify);
    OUGHT(tal::telling::way("iTerm.app") == Way::nine);
    OUGHT(tal::telling::way("WezTerm") == Way::nine);
    OUGHT(tal::telling::way("ghostty") == Way::nine);

    for (const char* other : {"", "xterm-256color", "vte", "tmux", "screen", "Apple_Terminal", "vscode", "mintty", "rxvt"})
        OUGHT(tal::telling::way(other) == Way::none);
}

// Control characters of C0 and C1 and DEL are left out, line breaks and tabs become spaces (line
// breaks stay when asked), and bytes that are not UTF-8 are repaired.
TRIAL("Telling: texts that cannot end a sequence") {
    SAME(tal::telling::shown("Poirot żółw", false), std::string{"Poirot żółw"});
    SAME(tal::telling::shown("a\x1b]9;x\x07" "b\x7f" "c\xc2\x9c" "d\xc2\xa0" "e", false), std::string{"a]9;xbcd\xc2\xa0" "e"});
    SAME(tal::telling::shown("one\ntwo\tthree\r", false), std::string{"one two three"});
    SAME(tal::telling::shown("one\ntwo\tthree", true), std::string{"one\ntwo three"});
    SAME(tal::telling::shown(std::string{"a\0b", 3}, true), std::string{"ab"});
    SAME(tal::telling::shown("a\x9c" "b", false), std::string{"a\xef\xbf\xbd" "b"});
}

// OSC 9 has one text, OSC 777 a title without a semicolon and a body, kitty's OSC 99 the title and
// the body in base64 under the notice's identifier, with the urgency.
TRIAL("Telling: the sequences") {
    const tal::Notice done{"Done", "All; 3 files\n", tal::Urgency::critical};

    SAME(tal::telling::sequence(done, Way::none, "1"), std::string{});
    SAME(tal::telling::sequence(done, Way::nine, "1"), std::string{"\x1b]9;Done: All; 3 files \x07"});
    SAME(tal::telling::sequence({"", "Body"}, Way::nine, "1"), std::string{"\x1b]9;Body\x07"});
    SAME(tal::telling::sequence({"Title", ""}, Way::nine, "1"), std::string{"\x1b]9;Title\x07"});
    SAME(tal::telling::sequence({"", "4;1;50"}, Way::nine, "1"), std::string{"\x1b]9; 4;1;50\x07"});
    SAME(tal::telling::sequence({"12", ""}, Way::nine, "1"), std::string{"\x1b]9;12\x07"});
    SAME(tal::telling::sequence({"", ";x"}, Way::nine, "1"), std::string{"\x1b]9;;x\x07"});
    SAME(tal::telling::sequence({"a;b", "c;d"}, Way::notify, "1"), std::string{"\x1b]777;notify;a,b;c;d\x07"});
    SAME(tal::telling::sequence(done, Way::kitty, "7"),
         std::string{"\x1b]99;i=7:d=0:u=2:e=1;RG9uZQ==\x1b\\\x1b]99;i=7:d=1:p=body:e=1;QWxsOyAzIGZpbGVzCg==\x1b\\"});
    SAME(tal::telling::sequence({"Hi", "", tal::Urgency::low}, Way::kitty, "8"), std::string{"\x1b]99;i=8:d=1:u=0:e=1;SGk=\x1b\\"});
    SAME(tal::telling::sequence({"Hi", "x"}, Way::kitty, "9").substr(0, 20), std::string{"\x1b]99;i=9:d=0:u=1:e=1"});
}

// The terminal's notifier writes on the loop, between its turns, wherever it was told, and holds
// what is told while the application is away; without a way, and once the application closed it,
// notices go to the notifier that stood before.
TRIAL("Telling: the terminal's notifier writes on its loop") {
    auto console = std::make_shared<tal::ScriptedConsole>();
    auto before = std::make_shared<tal::ScriptedNotifier>();
    tal::Loop loop{tal::Port<tal::Clock>{std::make_shared<tal::ManualClock>()}};
    tal::telling::Terminal nine{tal::Port<tal::Console>{console}, Way::nine, before, loop};

    SAME(nine.name(), std::string_view{"terminal"});
    OUGHT(nine.otherwise() == before);

    tal::wait(loop, nine.tell({"A", "1"}));
    tal::wait(loop, nine.tell({"B", "2"}));

    while (loop.turn()) {}

    SAME(console->written, std::string{"\x1b]9;A: 1\x07\x1b]9;B: 2\x07"});

    console->written.clear();

    std::thread elsewhere{[&nine] {
        tal::Loop other;

        tal::wait(other, nine.tell({"T", "thread"}));
    }};

    elsewhere.join();

    SAME(console->written, std::string{});

    while (loop.turn()) {}

    SAME(console->written, std::string{"\x1b]9;T: thread\x07"});

    console->written.clear();
    nine.away(true);
    tal::wait(loop, nine.tell({"W", "0"}));

    while (loop.turn()) {}

    SAME(console->written, std::string{});

    nine.away(false);

    SAME(console->written, std::string{"\x1b]9;W: 0\x07"});

    console->written.clear();
    tal::wait(loop, nine.tell({"C", "3"}));
    nine.closed();

    SAME(console->written, std::string{"\x1b]9;C: 3\x07"});

    tal::wait(loop, nine.tell({"D", "4"}));

    while (loop.turn()) {}

    SAME(console->written, std::string{"\x1b]9;C: 3\x07"});
    OUGHT(before->told == (std::vector<tal::Notice>{{"D", "4"}}));

    tal::telling::Terminal kitty{tal::Port<tal::Console>{console}, Way::kitty, before, loop};

    console->written.clear();
    tal::wait(loop, kitty.tell({"x", ""}));
    tal::wait(loop, kitty.tell({"y", ""}));

    while (loop.turn()) {}

    const std::string marks = console->written;
    const std::string mark = marks.substr(7, marks.find("-1:") - 7);

    OUGHT(marks.starts_with("\x1b]99;i=" + mark + "-1:d=1:"));
    OUGHT(marks.find("\x1b]99;i=" + mark + "-2:d=1:") != std::string::npos);
    OUGHT(!mark.empty() && mark.find_first_not_of("0123456789abcdef.") == std::string::npos);

    tal::telling::Terminal other{tal::Port<tal::Console>{console}, Way::kitty, before, loop};

    console->written.clear();
    tal::wait(loop, other.tell({"z", ""}));

    while (loop.turn()) {}

    OUGHT(console->written.starts_with("\x1b]99;i="));
    OUGHT(!console->written.starts_with("\x1b]99;i=" + mark + "-"));

    const std::string next = console->written.substr(7, console->written.find("-1:") - 7);
    const std::size_t dot = mark.find('.');

    OUGHT(dot != std::string::npos && next.find('.') != std::string::npos);
    SAME(std::stoull(next.substr(next.find('.') + 1), nullptr, 16), std::stoull(mark.substr(dot + 1), nullptr, 16) + 1);

    tal::telling::Terminal none{tal::Port<tal::Console>{console}, Way::none, before, loop};

    console->written.clear();
    tal::wait(loop, none.tell({"E", "5"}));

    while (loop.turn()) {}

    SAME(console->written, std::string{});
    OUGHT(before->told.back() == (tal::Notice{"E", "5"}));
}

// The application puts its terminal's notifier among the defaults for its lifetime and gives the
// one before back at its end (none when none was set), unless another was put there meanwhile; a
// terminal it knows no way for hands notices to that one. While the application steps aside,
// notices wait, and come after the terminal is dressed again.
TRIAL("Telling: the application's notifier") {
    auto before = std::make_shared<tal::ScriptedNotifier>();

    tal::defaults().set<tal::Notifier>(before);

    {
        auto console = std::make_shared<tal::ScriptedConsole>();

        console->environment["TERM_PROGRAM"] = "WezTerm";

        tal::App app{tal::Setup{}, tal::Port<tal::Console>{console}, tal::Port<tal::Clock>{std::make_shared<tal::ManualClock>()}};

        SAME(tal::Port<tal::Notifier>{}->name(), std::string_view{"terminal"});

        tal::wait(app.loop(), tal::tell({"Done", "ok"}));

        while (app.loop().turn()) {}

        OUGHT(console->written.find("\x1b]9;Done: ok\x07") != std::string::npos);
        OUGHT(before->told.empty());

        console->written.clear();

        {
            const tal::App::Pause aside{app};

            tal::wait(app.loop(), tal::tell({"Later", "ok"}));

            while (app.loop().turn()) {}

            OUGHT(console->written.find("]9;") == std::string::npos);
        }

        OUGHT(console->written.find("\x1b]9;Later: ok\x07") != std::string::npos);
        OUGHT(console->written.find("\x1b]9;Later: ok\x07") > console->written.rfind("\x1b[?25l"));
    }

    OUGHT(tal::Port<tal::Notifier>{}.shared() == before);

    {
        auto console = std::make_shared<tal::ScriptedConsole>();
        auto later = std::make_shared<tal::ScriptedNotifier>();

        console->environment["TERM_PROGRAM"] = "WezTerm";

        {
            tal::App app{tal::Setup{}, tal::Port<tal::Console>{console}, tal::Port<tal::Clock>{std::make_shared<tal::ManualClock>()}};

            tal::defaults().set<tal::Notifier>(later);
        }

        OUGHT(tal::Port<tal::Notifier>{}.shared() == later);
    }

    tal::defaults().set<tal::Notifier>(before);

    {
        auto console = std::make_shared<tal::ScriptedConsole>();

        console->environment["TERM"] = "xterm-256color";

        tal::App app{tal::Setup{}, tal::Port<tal::Console>{console}, tal::Port<tal::Clock>{std::make_shared<tal::ManualClock>()}};

        tal::wait(app.loop(), tal::tell({"Done", "ok"}));

        while (app.loop().turn()) {}

        OUGHT(console->written.find("]9;") == std::string::npos);
        OUGHT(before->told == (std::vector<tal::Notice>{{"Done", "ok"}}));
    }

    tal::defaults().clear<tal::Notifier>();

    {
        auto console = std::make_shared<tal::ScriptedConsole>();
        const tal::App app{tal::Setup{}, tal::Port<tal::Console>{console}, tal::Port<tal::Clock>{std::make_shared<tal::ManualClock>()}};

        OUGHT(tal::defaults().held<tal::Notifier>() != nullptr);
    }

    OUGHT(tal::defaults().held<tal::Notifier>() == nullptr);
}
