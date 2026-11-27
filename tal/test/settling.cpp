#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/json.hpp"
#include "tal/port.hpp"
#include "tal/settings.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "generating.hpp"
#include "mending.hpp"
#include "preset.hpp"

#include "tal/testing.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

// A program's settings: the built-in values of test/settings made into handles (the namespace
// `preset`), the person's deviations above them in a file only its owner reads, a file broken by
// hand kept untouched until it is repaired or restored, and the generator's refusals.

namespace {

using tal::Path;
using tal::Records;
using tal::Settings;

// The settings file of the trials, in memory.
constexpr std::string_view spot = "/config/settings.json";

// The copy a repair or a restore keeps of the original.
constexpr std::string_view sheltered = "/config/settings.json.broken";

// The files of a program in memory, with the loop that waits for the tasks.
struct Desk {
    tal::Loop loop;
    std::shared_ptr<tal::fs::Memory> memory = std::make_shared<tal::fs::Memory>();

    Desk() { tal::defaults().set<tal::fs::Files>(memory); }

    ~Desk() { tal::defaults().clear<tal::fs::Files>(); }

    Desk(const Desk&) = delete;
    Desk& operator=(const Desk&) = delete;

    // The settings of the trials, not read yet.
    [[nodiscard]] static Settings settings() { return Settings{preset::builtin(), Path{spot}}; }

    // Writes `text` as the file at `where`.
    void put(std::string_view text, std::string_view where = spot) {
        tal::wait(loop, tal::fs::make(Path{where}.parent()));
        tal::wait(loop, tal::fs::write(Path{where}, tal::bytes(text)));
    }

    // The text of the file at `where`, or "(none)".
    std::string text(std::string_view where = spot) {
        try {
            return tal::text::of(tal::wait(loop, tal::fs::read(Path{where})));
        } catch (const tal::Trouble&) {
            return "(none)";
        }
    }

    // The file at `where` as JSON, compact.
    std::string inside(std::string_view where = spot) { return tal::json::written(tal::json::parsed(text(where))); }

    // The rights of the file at `where`.
    std::optional<std::uint16_t> rights(std::string_view where) { return tal::wait(loop, tal::fs::look(Path{where})).rights; }

    // Waits for `work`, and says why it failed, or nothing.
    std::string outcome(tal::Task<void> work) {
        try {
            tal::wait(loop, std::move(work));
        } catch (const std::exception& why) {
            return why.what();
        }

        return {};
    }
};

// The problems of the generator of settings with `text` as the built-in values.
std::vector<std::string> generator(std::string_view text, std::string_view space = "preset") {
    std::vector<std::string> problems;

    (void)tal::generating::settled({"defaults.json", std::string{text}}, space, "preset.hpp", problems);

    return problems;
}

// One problem alone, or a word that there are not exactly one.
std::string alone(const std::vector<std::string>& problems) {
    if (problems.size() == 1) return problems.front();

    std::string made = std::to_string(problems.size()) + " problems:";

    for (const std::string& one : problems) made += "\n" + one;

    return made;
}

}

TRIAL("settings: the built-in values are there before anything is read") {
    Desk desk;
    const Settings one = Desk::settings();

    SAME(one.get(preset::share::port), 0);
    SAME(one.get(preset::share::limit), -1);
    SAME(one.get(preset::appearance::skin), std::string{"twilight-dark"});
    SAME(one.get(preset::panel::hidden), false);
    SAME(one.get(preset::panel::columns), (std::vector<std::string>{"name", "size", "written"}));
    SAME(one.get(preset::shortcuts::panels::delete_), std::string{"F8"});
    SAME(one.get(preset::shortcuts::panels::open_with), std::string{"Shift+Enter"});
    SAME(one.get(preset::shortcuts::editor::near_), std::string{});
    OUGHT(one.get(preset::mailboxes).empty());
    OUGHT(one.get(preset::share::blocked).empty());
    OUGHT(!one.damaged());
    OUGHT(one.where() == Path{spot});
}

TRIAL("settings: read starts afresh and drops what the file does not have") {
    Desk desk;
    Settings one = Desk::settings();

    desk.put(R"({"share": {"port": 7000}})");
    one.set(preset::panel::hidden, true);

    SAME(desk.outcome(one.read()), std::string{});
    SAME(one.get(preset::share::port), 7000);
    SAME(one.get(preset::panel::hidden), false);
    OUGHT(!one.changed(preset::panel::hidden));
    OUGHT(one.changed(preset::share::port));
    OUGHT(!one.damaged());
}

TRIAL("settings: a missing file, or a folder in its place, is a first run") {
    Desk desk;
    Settings one = Desk::settings();

    one.set(preset::panel::hidden, true);
    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(!one.damaged());
    OUGHT(!one.changed(preset::panel::hidden));

    tal::wait(desk.loop, tal::fs::make(Path{spot}));
    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(!one.damaged());
}

TRIAL("settings: set overrides, builtin still gives the built-in value, reset drops the deviation") {
    Desk desk;
    Settings one = Desk::settings();

    OUGHT(!one.changed(preset::share::port));

    one.set(preset::share::port, 5000);

    SAME(one.get(preset::share::port), 5000);
    SAME(one.builtin(preset::share::port), 0);
    OUGHT(one.changed(preset::share::port));
    OUGHT(!one.changed(preset::share::limit));

    one.set(preset::share::port, 0);

    OUGHT(one.changed(preset::share::port));

    one.reset(preset::share::port);

    OUGHT(!one.changed(preset::share::port));
    SAME(one.get(preset::share::port), 0);

    one.set(preset::share::port, 5000);
    one.set(preset::appearance::skin, std::string{"wieczor"});
    one.reset();

    OUGHT(!one.changed(preset::share::port));
    OUGHT(!one.changed(preset::appearance::skin));
    SAME(one.get(preset::appearance::skin), std::string{"twilight-dark"});
}

TRIAL("settings: the file holds only the difference, and a reset leaves no empty object") {
    Desk desk;
    Settings one = Desk::settings();

    one.set(preset::appearance::skin, std::string{"wieczor"});
    one.set(preset::shortcuts::panels::delete_, std::string{"Del"});
    one.set(preset::shortcuts::editor::near_, std::string{"Alt+N"});
    one.reset(preset::shortcuts::editor::near_);

    SAME(desk.outcome(one.write()), std::string{});
    SAME(desk.inside(), std::string{R"({"appearance":{"skin":"wieczor"},"shortcuts":{"panels":{"delete":"Del"}}})"});
    OUGHT(desk.text().ends_with("}\n"));

    one.reset(preset::shortcuts::panels::delete_);

    SAME(desk.outcome(one.write()), std::string{});
    SAME(desk.inside(), std::string{R"({"appearance":{"skin":"wieczor"}})"});

    one.reset(preset::appearance::skin);

    SAME(desk.outcome(one.write()), std::string{});
    SAME(desk.inside(), std::string{"{}"});
}

TRIAL("settings: a value of another shape in the file gives way to the built-in one") {
    Desk desk;
    Settings one = Desk::settings();

    desk.put(R"({"share":{"port":"duzo","limit":2.9,"blocked":["*.exe",7]},"panel":{"hidden":true,"guard":1e10,"columns":"name"},)"
             R"("appearance":7,"mailboxes":[{"name":"a","here":1}],"shortcuts":{"panels":{"delete":null}}})");

    SAME(desk.outcome(one.read()), std::string{});

    SAME(one.get(preset::share::port), 0);
    SAME(one.get(preset::share::limit), 2);
    OUGHT(one.get(preset::share::blocked).empty());
    SAME(one.get(preset::panel::hidden), true);
    SAME(one.get(preset::panel::guard), 750);
    SAME(one.get(preset::panel::columns).size(), std::size_t{3});
    SAME(one.get(preset::appearance::skin), std::string{"twilight-dark"});
    OUGHT(one.get(preset::mailboxes).empty());
    SAME(one.get(preset::shortcuts::panels::delete_), std::string{"F8"});
    OUGHT(one.changed(preset::share::port));

    desk.put(R"({"panel":{"guard":"x","hidden":"x"},"mailboxes":[{"name":"a"},7]})");
    SAME(desk.outcome(one.read()), std::string{});
    SAME(one.get(preset::panel::guard), 750);
    SAME(one.get(preset::panel::hidden), false);
    OUGHT(one.get(preset::mailboxes).empty());

    desk.put(R"({"share":{"limit":-2147483648,"port":2147483647}})");
    SAME(desk.outcome(one.read()), std::string{});
    SAME(one.get(preset::share::limit), -2147483647 - 1);
    SAME(one.get(preset::share::port), 2147483647);

    desk.put(R"({"share":{"limit":-2147483649,"port":2147483648}})");
    SAME(desk.outcome(one.read()), std::string{});
    SAME(one.get(preset::share::limit), -1);
    SAME(one.get(preset::share::port), 0);

    desk.put(R"({"share":{"limit":-2147483648.9,"port":2147483647.9}})");
    SAME(desk.outcome(one.read()), std::string{});
    SAME(one.get(preset::share::limit), -2147483647 - 1);
    SAME(one.get(preset::share::port), 2147483647);

    desk.put(R"({"share":{"limit":-2147483649.0,"port":2147483648.0}})");
    SAME(desk.outcome(one.read()), std::string{});
    SAME(one.get(preset::share::limit), -1);
    SAME(one.get(preset::share::port), 0);
}

TRIAL("settings: a key the program does not know survives a write; set goes through what stands in the way") {
    Desk desk;
    Settings one = Desk::settings();

    desk.put(R"({"nowe":{"cos":1},"shortcuts":"x"})");
    SAME(desk.outcome(one.read()), std::string{});

    one.set(preset::shortcuts::panels::delete_, std::string{"Del"});

    SAME(desk.outcome(one.write()), std::string{});
    SAME(desk.inside(), std::string{R"({"nowe":{"cos":1},"shortcuts":{"panels":{"delete":"Del"}}})"});
}

TRIAL("settings: read reverts what was not written; every type comes back as what it is") {
    Desk desk;
    Settings one = Desk::settings();
    const Records boxes{{{"name", "Filmy"}, {"here", "/media/films"}}, {{"name", "Muzyka"}}};

    one.set(preset::appearance::skin, std::string{"poranek"});
    one.set(preset::share::port, 5000);
    one.set(preset::panel::hidden, true);
    one.set(preset::panel::columns, std::vector<std::string>{"name", "size"});
    one.set(preset::share::blocked, std::vector<std::string>{});
    one.set(preset::mailboxes, boxes);

    SAME(desk.outcome(one.write()), std::string{});

    one.set(preset::appearance::skin, std::string{"wieczor"});
    one.reset();

    OUGHT(one.get(preset::mailboxes).empty());
    SAME(desk.outcome(one.read()), std::string{});

    SAME(one.get(preset::appearance::skin), std::string{"poranek"});
    SAME(one.get(preset::share::port), 5000);
    SAME(one.get(preset::panel::hidden), true);
    SAME(one.get(preset::panel::columns), (std::vector<std::string>{"name", "size"}));
    OUGHT(one.get(preset::share::blocked).empty() && one.changed(preset::share::blocked));
    OUGHT(one.get(preset::mailboxes) == boxes);
    OUGHT(desk.inside().find(R"("mailboxes":[{"here":"/media/films","name":"Filmy"},{"name":"Muzyka"}])") != std::string::npos);
}

TRIAL("settings: the file and its copy are for the owner alone") {
    Desk desk;
    Settings one = Desk::settings();

    desk.put("{}");
    tal::wait(desk.loop, tal::fs::change(Path{spot}, tal::fs::Changing{.attributes = {}, .modified = {}, .rights = 0644}));
    one.set(preset::appearance::skin, std::string{"wieczor"});

    SAME(desk.outcome(one.write()), std::string{});
    OUGHT(desk.rights(spot) == std::optional<std::uint16_t>{0600});

    SAME(desk.outcome(one.restore()), std::string{});
    OUGHT(desk.rights(sheltered) == std::optional<std::uint16_t>{0600});
}

TRIAL("settings: a broken file is damaged, and writing leaves it alone") {
    Desk desk;
    Settings one = Desk::settings();
    const std::string broken = R"({"appearance": {"skin": "wieczor",}, "keys": {"tmdb": "tajne"})";

    desk.put(broken);
    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(one.damaged());
    SAME(one.get(preset::appearance::skin), std::string{"twilight-dark"});

    one.set(preset::panel::hidden, true);

    SAME(desk.outcome(one.write()), std::string{"The settings file is damaged and stays untouched."});
    SAME(desk.text(), broken);

    for (const std::string& text : {std::string{"[]"}, std::string{"\"x\""}, std::string{"{\"a\":\"\xC3\x28\"}"}, std::string(tal::json::Limits{}.bytes + 1, ' ')}) {
        desk.put(text);
        SAME(desk.outcome(one.read()), std::string{});
        OUGHT(one.damaged());
    }

    std::string large = R"({"share":{"port":5}})";

    large.resize(tal::json::Limits{}.bytes, ' ');
    desk.put(large);
    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(!one.damaged());
    SAME(one.get(preset::share::port), 5);

    desk.put(large + " ");
    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(one.damaged());

    tal::wait(desk.loop, tal::fs::remove(Path{spot}));
    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(!one.damaged());
    SAME(desk.outcome(one.write()), std::string{});
}

// Built-in values with a version give it to the file before the deviations; a file without one,
// or of an older one, is read as it is, and one of a newer version is read but never written over,
// unless the person starts again from the built-in values.
TRIAL("settings: the version of the built-in values goes to the file, a newer file stays untouched") {
    Desk desk;
    Settings two{R"({"version": 2, "a": {"b": 1}})", Path{spot}};
    constexpr tal::Setting<int> b{"a.b"};

    SAME(desk.outcome(two.read()), std::string{});
    OUGHT(!two.newer());

    two.set(b, 5);

    SAME(desk.outcome(two.write()), std::string{});
    SAME(desk.inside(), std::string{R"({"version":2,"a":{"b":5}})"});

    for (const std::string_view older : {R"({"a":{"b":7}})", R"({"version":1,"a":{"b":7}})", R"({"version":"3","a":{"b":7}})", R"({"version":-4,"a":{"b":7}})",
                                         R"({"version":-4294967291,"a":{"b":7}})"}) {
        desk.put(older);
        SAME(desk.outcome(two.read()), std::string{});
        OUGHT(!two.newer());
        SAME(two.get(b), 7);
        SAME(desk.outcome(two.write()), std::string{});
        SAME(desk.inside(), std::string{R"({"version":2,"a":{"b":7}})"});
    }

    const std::string newer = R"({"version": 3, "a": {"b": 9}, "z": {"y": "new"}})";

    desk.put(newer);
    SAME(desk.outcome(two.read()), std::string{});
    OUGHT(two.newer());
    OUGHT(!two.damaged());
    SAME(two.get(b), 9);
    SAME(desk.outcome(two.write()), std::string{"The settings file comes from a newer version and stays untouched."});
    SAME(desk.text(), newer);

    desk.put(R"({"version": 2, "a": {"b": 6}})");
    SAME(desk.outcome(two.read()), std::string{});
    OUGHT(!two.newer());
    SAME(two.get(b), 6);

    desk.put(newer);
    SAME(desk.outcome(two.read()), std::string{});
    SAME(desk.outcome(two.restore()), std::string{});
    OUGHT(!two.newer());
    SAME(desk.inside(), std::string{R"({"version":2})"});

    const std::string broken = R"({"version": 3, "a": {"b": 4},})";

    desk.put(broken);
    SAME(desk.outcome(two.read()), std::string{});
    OUGHT(two.damaged());
    SAME(desk.outcome(two.repair()), std::string{});
    OUGHT(!two.damaged());
    OUGHT(two.newer());
    SAME(two.get(b), 4);
    SAME(desk.text(), broken);

    desk.put(R"({"version": 1, "a": {"b": 3},})");
    SAME(desk.outcome(two.read()), std::string{});
    SAME(desk.outcome(two.repair()), std::string{});
    OUGHT(!two.newer());
    SAME(desk.inside(), std::string{R"({"version":2,"a":{"b":3}})"});

    Settings plain = Desk::settings();

    SAME(desk.outcome(plain.restore()), std::string{});
    SAME(desk.inside(), std::string{"{}"});
}

TRIAL("settings: repair removes comments, commas and the BOM, the original stays beside it") {
    Desk desk;
    Settings one = Desk::settings();
    const std::string broken = "\xEF\xBB\xBF{\n"
                               "  // skórka na wieczór\n"
                               "  \"appearance\": {\"skin\": \"wieczor\", /* było: sand */},\n"
                               "  \"keys\": {\"tmdb\": \"tajne // nie komentarz\",},\n"
                               "  \"panel\": {\"columns\": [\"name\", \"size\",],},\n"
                               "}\n";

    desk.put(broken);
    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(one.damaged());

    SAME(desk.outcome(one.repair()), std::string{});
    OUGHT(!one.damaged());
    SAME(one.get(preset::appearance::skin), std::string{"wieczor"});
    SAME(one.get(preset::keys::tmdb), std::string{"tajne // nie komentarz"});
    SAME(one.get(preset::panel::columns), (std::vector<std::string>{"name", "size"}));
    SAME(desk.text(sheltered), broken);

    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(!one.damaged());
    SAME(one.get(preset::appearance::skin), std::string{"wieczor"});
}

TRIAL("settings: repair closes a cut-off file and drops a cut-off pair") {
    Desk desk;
    Settings one = Desk::settings();
    const std::vector<std::pair<std::string, std::string>> cases{
        {R"({"appearance": {"skin": "wiec)", "wiec"},
        {R"({"appearance": {"skin": "wieczor"}, "panel": {"hidden": tru)", "wieczor"},
        {R"({"appearance": {"skin": "wieczor", "language":)", "wieczor"},
        {R"({"appearance": {"skin": "wieczor", "lang)", "wieczor"},
        {R"({"appearance": {"skin": "wieczor"}, "panel": {"columns": ["name", "si)", "wieczor"},
        {R"({"appearance": {"skin": "a\)", "a"},
        {R"({"appearance": {"skin": "wieczor"}} /* koniec)", "wieczor"},
    };

    for (const auto& [given, skin] : cases) {
        desk.put(given);
        SAME(desk.outcome(one.read()), std::string{});
        OUGHT(one.damaged());
        SAME(desk.outcome(one.repair()), std::string{});
        OUGHT(!one.damaged());
        SAME(one.get(preset::appearance::skin), skin);
        SAME(one.get(preset::panel::hidden), false);
        SAME(one.get(preset::appearance::language), std::string{"auto"});
    }

    desk.put(R"({"panel": {"hidden": true)");
    SAME(desk.outcome(one.read()), std::string{});
    SAME(desk.outcome(one.repair()), std::string{});
    SAME(one.get(preset::panel::hidden), true);

    desk.put(R"({"panel": {"columns": ["name", "size", nul)");
    SAME(desk.outcome(one.read()), std::string{});
    SAME(desk.outcome(one.repair()), std::string{});
    SAME(one.get(preset::panel::columns), (std::vector<std::string>{"name", "size"}));

    desk.put(R"({"panel": {"columns": ["name", "si)");
    SAME(desk.outcome(one.read()), std::string{});
    SAME(desk.outcome(one.repair()), std::string{});
    SAME(one.get(preset::panel::columns), (std::vector<std::string>{"name", "si"}));
}

TRIAL("settings: repair refuses what it cannot repair without guessing, and touches nothing") {
    Desk desk;
    Settings one = Desk::settings();

    for (const std::string given : {"to nie jest JSON", "[1, 2, 3]", "{\"a\" 1}", R"({"appearance": {"skin": "x", "language": -tru)"}) {
        desk.put(given);
        SAME(desk.outcome(one.read()), std::string{});

        const std::string said = desk.outcome(one.repair());

        OUGHT(said.starts_with("The settings file cannot be repaired: "));
        OUGHT(one.damaged());
        SAME(desk.text(), given);
        SAME(desk.text(sheltered), std::string{"(none)"});
    }

    tal::wait(desk.loop, tal::fs::remove(Path{spot}));
    OUGHT(!desk.outcome(one.repair()).empty());
    OUGHT(!desk.outcome(one.restore()).empty());
}

TRIAL("settings: restore keeps the original beside it and starts from the built-in values") {
    Desk desk;
    Settings one = Desk::settings();
    const std::string given = R"({"appearance": {"skin": "wieczor"}, "keys": {"tmdb": "tajne")";

    desk.put(given);
    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(one.damaged());

    one.set(preset::panel::hidden, true);

    SAME(desk.outcome(one.restore()), std::string{});
    OUGHT(!one.damaged());
    SAME(one.get(preset::appearance::skin), std::string{"twilight-dark"});
    SAME(one.get(preset::panel::hidden), false);
    SAME(desk.text(sheltered), given);
    SAME(desk.inside(), std::string{"{}"});
}

TRIAL("settings: a BOM at the start reads without repair") {
    Desk desk;
    Settings one = Desk::settings();

    desk.put("\xEF\xBB\xBF{\"appearance\": {\"skin\": \"wieczor\"}}");
    SAME(desk.outcome(one.read()), std::string{});
    OUGHT(!one.damaged());
    SAME(one.get(preset::appearance::skin), std::string{"wieczor"});
}

TRIAL("settings: writes go in the order asked, each with the values of its call") {
    Desk desk;
    Settings one = Desk::settings();
    std::vector<tal::Task<void>> writes;

    for (int at = 1; at <= 20; ++at) {
        one.set(preset::share::port, at);
        writes.push_back(one.write());
    }

    one.set(preset::share::port, 99);

    for (int at = 19; at >= 0; --at) SAME(desk.outcome(std::move(writes[static_cast<std::size_t>(at)])), std::string{});

    SAME(desk.inside(), std::string{R"({"share":{"port":20}})"});

    one.set(preset::share::port, 21);

    tal::Task<void> dropped = one.write();

    one.set(preset::share::port, 22);
    SAME(desk.outcome(one.write()), std::string{});
    SAME(desk.inside(), std::string{R"({"share":{"port":22}})"});
}

TRIAL("settings: a value that is not UTF-8 makes the write fail, and the file stays") {
    Desk desk;
    Settings one = Desk::settings();

    one.set(preset::appearance::skin, std::string{"wieczor"});
    SAME(desk.outcome(one.write()), std::string{});

    one.set(preset::appearance::skin, std::string{"\xC3\x28"});
    OUGHT(!desk.outcome(one.write()).empty());
    SAME(desk.inside(), std::string{R"({"appearance":{"skin":"wieczor"}})"});
}

TRIAL("settings: built-in values that are not an object, or a handle they lack, are a mistake of the program") {
    THREW(Settings("[]", Path{spot}));
    THREW(Settings("{", Path{spot}));

    const Settings one{R"({"a":{"b":1}})", Path{spot}};

    THREW((void)one.get(tal::Setting<int>{"a.c"}));
    THREW((void)one.get(tal::Setting<std::string>{"a.b"}));
    SAME(one.get(tal::Setting<int>{"a.b"}), 1);
}

TRIAL("settings: no damage to a file makes get throw") {
    Desk desk;
    Settings one = Desk::settings();
    std::mt19937 random{537};
    const std::string whole =
        R"({"appearance":{"skin":"wieczor","language":"pl"},"panel":{"columns":["name"],"hidden":true,"guard":5},"shortcuts":{"panels":)"
        R"({"delete":"Del","open-with":"Enter"},"editor":{"near":"x"}},"mailboxes":[{"name":"a"}],"keys":{"tmdb":"k"},)"
        R"("share":{"port":1,"limit":2,"blocked":["*.exe"]},"revision":{"number":1}})";

    for (int round = 0; round < 2000; ++round) {
        std::string damaged = whole;
        const int much = 1 + static_cast<int>(random() % 4);

        for (int step = 0; step < much && !damaged.empty(); ++step) {
            const std::size_t at = random() % damaged.size();
            const char pieces[] = {'{', '}', '"', ',', ':', '[', ']', '1', '.', 'e', '-', 'x', '\\'};

            if (random() % 2 == 0) damaged.erase(at, 1 + random() % 6);
            else damaged.insert(at, 1, pieces[random() % sizeof pieces]);
        }

        desk.put(damaged);

        SAME(desk.outcome(one.read()), std::string{});

        (void)one.get(preset::appearance::skin);
        (void)one.get(preset::appearance::language);
        (void)one.get(preset::panel::columns);
        (void)one.get(preset::panel::hidden);
        (void)one.get(preset::panel::guard);
        (void)one.get(preset::shortcuts::panels::delete_);
        (void)one.get(preset::shortcuts::panels::open_with);
        (void)one.get(preset::shortcuts::editor::near_);
        (void)one.get(preset::mailboxes);
        (void)one.get(preset::keys::tmdb);
        (void)one.get(preset::share::port);
        (void)one.get(preset::share::limit);
        (void)one.get(preset::share::blocked);
        (void)one.get(preset::revision::number);

        if (one.damaged()) (void)desk.outcome(one.repair());
    }
}

TRIAL("settings: mended leaves good JSON as it is") {
    for (const std::string_view text : {R"({})", R"({"a":[1,2,{"b":"c // d"}],"e":null,"f":true,"g":-1.5e3})", R"([])", "{\"a\":\"\\\"}\"}"})
        SAME(tal::mended(text), std::string{text});
}

TRIAL("settings: the generator gives each value a type and refuses what has none") {
    SAME(alone(generator(R"({"a":{"b":null}})")), std::string{R"(defaults.json:1:11: "a.b" is null, which has no type)"});
    SAME(alone(generator(R"({"a":{"b":1.5}})")), std::string{R"(defaults.json:1:11: "a.b" has a fraction; a setting holds whole numbers)"});
    SAME(alone(generator(R"({"a":{"b":4294967296}})")), std::string{R"(defaults.json:1:11: "a.b" does not fit an int)"});
    SAME(alone(generator(R"({"a":{"b":-2147483649}})")), std::string{R"(defaults.json:1:11: "a.b" does not fit an int)"});
    SAME(alone(generator(R"({"a":{"b":[1]}})")), std::string{R"(defaults.json:1:11: "a.b" is a list of something else than texts)"});
    SAME(alone(generator(R"({"a":{}})")), std::string{R"(defaults.json:1:6: "a" holds no settings)"});
    SAME(alone(generator(R"({"a":{"b":{}}})")), std::string{R"(defaults.json:1:11: "a.b" holds no settings)"});
    SAME(alone(generator(R"({"a":"x"})")), std::string{R"(defaults.json:1:6: "a" is a section: an object of settings, or a list of records)"});
    SAME(alone(generator(R"({"a":[{"x":1}]})")), std::string{R"(defaults.json:1:6: "a" is a list of records, which are objects of texts)"});
    SAME(alone(generator(R"({"a":["x"]})")), std::string{R"(defaults.json:1:6: "a" is a list of records, which are objects of texts)"});
    SAME(alone(generator(R"({"builtin":{"x":1}})")), std::string{R"(defaults.json:1:12: "builtin" would hide preset::builtin())"});
    SAME(alone(generator(R"({"a":{"b-c":1,"b_c":2}})")), std::string{R"(defaults.json:1:21: "a.b-c" and "a.b_c" are both a::b_c in C++)"});
    SAME(alone(generator(R"({"a":{"B":1}})")), std::string{R"(defaults.json:1:11: "a.B": a name is small letters, digits, _ and -, from a letter)"});
    SAME(alone(generator(R"([])")), std::string{"defaults.json:1:1: the settings are not an object"});
    SAME(alone(generator(R"({"a":)")), std::string{"defaults.json:1:6: a value is missing"});
    SAME(alone(generator(R"({"a":{"b":1}})", "std")), std::string{R"(tal-settings: "std" cannot be the namespace of the settings)"});
    SAME(alone(generator(R"({"version":0,"a":{"b":1}})")), std::string{R"(defaults.json:1:12: "version" is the version of the file, a whole number from 1)"});
    SAME(alone(generator(R"({"version":"2","a":{"b":1}})")), std::string{R"(defaults.json:1:12: "version" is the version of the file, a whole number from 1)"});
    OUGHT(generator(R"({"version":2,"a":{"version":"x"}})").empty());

    std::vector<std::string> problems;
    const tal::generating::Made made = tal::generating::settled(
        {"defaults.json", R"({"a":{"b":true,"c":-7,"d":"x","e":[],"f":{"delete":["y"]}},"g":[],"h":[{"x":"y"}]})"}, "my::set", "set.hpp", problems);

    OUGHT(problems.empty());
    OUGHT(made.header.find("namespace my::set::a {\n\ninline constexpr ::tal::Setting<bool> b{\"a.b\"};\n"
                           "inline constexpr ::tal::Setting<int> c{\"a.c\"};\n"
                           "inline constexpr ::tal::Setting<::std::string> d{\"a.d\"};\n"
                           "inline constexpr ::tal::Setting<::std::vector<::std::string>> e{\"a.e\"};\n\n}\n") != std::string::npos);
    OUGHT(made.header.find("namespace my::set::a::f {\n\ninline constexpr ::tal::Setting<::std::vector<::std::string>> delete_{\"a.f.delete\"};\n") !=
          std::string::npos);
    OUGHT(made.header.find("namespace my::set {\n\ninline constexpr ::tal::Setting<::tal::Records> g{\"g\"};\n"
                           "inline constexpr ::tal::Setting<::tal::Records> h{\"h\"};\n") != std::string::npos);
    OUGHT(made.code.starts_with("#include \"set.hpp\"\n"));

    const tal::generating::Made versioned = tal::generating::settled({"defaults.json", R"({"version":4,"a":{"b":true}})"}, "my::set", "set.hpp", problems);

    OUGHT(problems.empty());
    OUGHT(versioned.header.find("version") == std::string::npos);
    OUGHT(versioned.header.find("Setting<bool> b{\"a.b\"}") != std::string::npos);
    OUGHT(generator(R"({"a":{"b":-2147483648,"c":2147483647}})").empty());
}

#ifndef _WIN32

TRIAL("settings: on the disk the file is for its owner alone, and a link to it stays a link") {
    tal::Loop loop;
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "tal-settings-trial";
    std::error_code why;

    std::filesystem::remove_all(folder, why);
    std::filesystem::create_directories(folder / "real", why);

    const auto path_of = [](const std::filesystem::path& one) { return Path{one.string()}; };

    Settings one{preset::builtin(), path_of(folder / "settings.json")};

    one.set(preset::share::port, 1);
    tal::wait(loop, one.write());

    struct stat told{};

    SAME(::stat((folder / "settings.json").c_str(), &told), 0);
    SAME(told.st_mode & static_cast<::mode_t>(0777), static_cast<::mode_t>(0600));

    std::filesystem::create_symlink(folder / "real" / "kept.json", folder / "linked.json", why);

    Settings linked{preset::builtin(), path_of(folder / "linked.json")};

    { std::ofstream{folder / "real" / "kept.json"} << "{}"; }

    linked.set(preset::share::port, 2);
    tal::wait(loop, linked.write());

    OUGHT(std::filesystem::is_symlink(folder / "linked.json"));
    tal::wait(loop, linked.read());
    SAME(linked.get(preset::share::port), 2);
    SAME(::stat((folder / "real" / "kept.json").c_str(), &told), 0);
    SAME(told.st_mode & static_cast<::mode_t>(0777), static_cast<::mode_t>(0600));

    const ::mode_t before = ::umask(0277);

    one.set(preset::share::port, 3);
    tal::wait(loop, one.write());
    ::umask(before);

    SAME(::stat((folder / "settings.json").c_str(), &told), 0);
    SAME(told.st_mode & static_cast<::mode_t>(0777), static_cast<::mode_t>(0600));

    std::filesystem::remove_all(folder, why);
}

#endif
