#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/json.hpp"
#include "tal/port.hpp"
#include "tal/text.hpp"
#include "tal/texts.hpp"
#include "tal/trouble.hpp"
#include "tal/wording.hpp"

#include "generating.hpp"
#include "spoken.hpp"
#include "texts_of.hpp"

#include "tal/testing.hpp"

// A program's texts: the functions the generator makes (here of test/texts, in the namespace
// `spoken`), the dictionary under them with English compiled in and a language read over it, a
// translation from outside taken only where it fits, and the generator's own refusals and checks.

namespace {

using tal::Path;
using tal::Texts;

// English of the trials that make a dictionary of their own.
constexpr std::string_view english =
    R"({"meta":{"name":"English"},"title":"Trials","menu":{"left":{"text":"Left","key":"L"},"quit":"Quit"},)"
    R"("panel":{"marked":{"one":"{} file marked","other":"{} files marked"},"moved":"Moved {} to {}"}})";

// The keys of `english`, in its order.
constexpr std::string_view keys[] = {"meta.name", "title", "menu.left.text", "menu.left.key", "menu.quit", "panel.marked", "panel.moved"};

// The places of the texts of `english` among its keys.
enum Key : std::size_t { name, title, left, key, quit, marked, moved };

// A Polish translation of `english` that fits wholly.
constexpr std::string_view polish =
    R"({"meta":{"name":"Polski"},"title":"Próby","menu":{"left":{"text":"Lewy","key":"Ł"},"quit":"Wyjście"},)"
    R"("panel":{"marked":{"one":"{} plik","few":"{} pliki","many":"{} plików"},"moved":"Przeniesiono {} do {}"}})";

// The files of a program in memory: the user's folder and the program's, and the loop that waits
// for the tasks.
struct Desk {
    tal::Loop loop;
    std::shared_ptr<tal::fs::Memory> memory = std::make_shared<tal::fs::Memory>();

    Desk() {
        tal::defaults().set<tal::fs::Files>(memory);
        tal::wait(loop, tal::fs::make(Path{"/user"}));
        tal::wait(loop, tal::fs::make(Path{"/program"}));
    }

    ~Desk() { tal::defaults().clear<tal::fs::Files>(); }

    Desk(const Desk&) = delete;
    Desk& operator=(const Desk&) = delete;

    void put(std::string_view where, std::string_view text) { tal::wait(loop, tal::fs::write(Path{where}, tal::bytes(text))); }

    void read(Texts& texts, std::string code) { tal::wait(loop, texts.read({Path{"/user"}, Path{"/program"}}, std::move(code))); }
};

// A dictionary of `english`.
Texts made() { return Texts{keys, english}; }

// The problems the generator finds in `text`, given as the English file "en-US.json".
std::vector<std::string> generator(std::string_view text, std::string_view space = "spoken") {
    std::vector<std::string> problems;

    (void)tal::generating::generated({"en-US.json", std::string{text}}, space, "spoken.hpp", problems);

    return problems;
}

// The problems of the language `name` with `text` beside `english`.
std::vector<std::string> checked(std::string name, std::string_view text) {
    return tal::generating::checked({"en-US.json", std::string{english}}, {{std::move(name), std::string{text}}});
}

// One problem alone, or a word that there are not exactly one.
std::string alone(const std::vector<std::string>& problems) {
    if (problems.size() == 1) return problems.front();

    std::string made = std::to_string(problems.size()) + " problems:";

    for (const std::string& one : problems) made += "\n" + one;

    return made;
}

// The number of {} in a text the dictionary gives.
int values(const std::string& said) { return tal::wording::slots(said); }

}

// The files of test/texts, carried in the binary (`texts_of`), are read from the program's folder
// on a disk in memory, so that the trial reads them wherever the binary runs.
TRIAL("texts: the generated functions speak English from the start, Polish after it is read") {
    Desk desk;

    SAME(spoken::dictionary().code(), std::string{"en-US"});
    SAME(spoken::title(), std::string{"Trials"});
    SAME(spoken::menu::left::text(), std::string{"Left"});
    SAME(spoken::menu::left::key(), U'L');
    SAME(spoken::menu::no_library(), std::string{"Make a library first"});
    SAME(spoken::menu::delete_(), std::string{"Delete"});
    SAME(spoken::menu::near_(), std::string{"Near"});
    SAME(spoken::panel::key(), std::string{"Key:"});
    SAME(spoken::panel::marked(1, "1"), std::string{"1 file marked"});
    SAME(spoken::panel::marked(0, "no"), std::string{"no files marked"});
    SAME(spoken::panel::moved("a.txt", "/b"), std::string{"Moved a.txt to /b"});
    SAME(spoken::panel::copied(2, "2", "/b"), std::string{"Copied 2 files to /b"});
    SAME(spoken::letters::accent::key(), U'Å');

    desk.put("/program/en-US.json", texts_of::en_US);
    desk.put("/program/pl-PL.json", texts_of::pl_PL);

    const std::vector<Path> folders{Path{"/program"}};

    tal::wait(desk.loop, spoken::dictionary().read(folders, "pl-PL"));

    SAME(spoken::dictionary().code(), std::string{"pl-PL"});
    SAME(spoken::title(), std::string{"Próby"});
    SAME(spoken::panel::marked(1, "1"), std::string{"1 plik zaznaczony"});
    SAME(spoken::panel::marked(3, "3"), std::string{"3 pliki zaznaczone"});
    SAME(spoken::panel::marked(12, "12"), std::string{"12 plików zaznaczonych"});
    SAME(spoken::panel::marked(22, "22"), std::string{"22 pliki zaznaczone"});
    SAME(spoken::panel::copied(5, "5", "/b"), std::string{"Skopiowano 5 plików do /b"});
    SAME(spoken::letters::accent::key(), U'Ą');

    const std::vector<tal::Spoken> languages = tal::wait(desk.loop, spoken::dictionary().languages(folders));

    SAME(languages.size(), std::size_t{2});
    OUGHT(languages.size() == 2 && languages[0] == tal::Spoken{"en-US", "English"} && languages[1] == tal::Spoken{"pl-PL", "Polski"});

    tal::wait(desk.loop, spoken::dictionary().read(folders, "en-US"));

    SAME(spoken::dictionary().code(), std::string{"en-US"});
    SAME(spoken::title(), std::string{"Trials"});
}

TRIAL("texts: a language whose file is missing, cannot be read or is not an object leaves English") {
    Desk desk;
    Texts texts = made();

    desk.put("/program/pl-PL.json", polish);
    desk.read(texts, "pl-PL");
    SAME(texts.said(title), std::string{"Próby"});

    desk.read(texts, "de-DE");
    SAME(texts.code(), std::string{"en-US"});
    SAME(texts.said(title), std::string{"Trials"});

    desk.put("/program/fr-FR.json", R"({"title": )");
    desk.read(texts, "fr-FR");
    SAME(texts.code(), std::string{"en-US"});

    desk.put("/program/it-IT.json", R"(["title"])");
    desk.read(texts, "it-IT");
    SAME(texts.code(), std::string{"en-US"});

    desk.put("/program/es-ES.json", "{\"title\":\"\xC3\x28\"}");
    desk.read(texts, "es-ES");
    SAME(texts.code(), std::string{"en-US"});

    std::string large = R"({"title":"Proeven"})";

    large.resize(tal::json::Limits{}.bytes, ' ');
    desk.put("/program/nl-NL.json", large);
    desk.read(texts, "nl-NL");
    SAME(texts.code(), std::string{"nl-NL"});
    SAME(texts.said(title), std::string{"Proeven"});

    desk.put("/program/nl-NL.json", large + " ");
    desk.read(texts, "nl-NL");
    SAME(texts.code(), std::string{"en-US"});

    tal::wait(desk.loop, tal::fs::make(Path{"/user/pl-PL.json"}));
    desk.read(texts, "pl-PL");
    SAME(texts.code(), std::string{"en-US"});

    tal::wait(desk.loop, texts.read({Path{"/nowhere"}}, "pl-PL"));
    SAME(texts.code(), std::string{"en-US"});
}

TRIAL("texts: a code that cannot name a file reads nothing") {
    Desk desk;
    Texts texts = made();

    desk.put("/program/pl-PL.json", polish);
    desk.put("/program/x.json", polish);

    for (const std::string& code : std::vector<std::string>{"../program/pl-PL", "pl/PL", "pl-PL.json", "", "1pl", "-pl", "pl PL", std::string(36, 'a')}) {
        desk.read(texts, "pl-PL");
        desk.read(texts, code);
        SAME(texts.code(), std::string{"en-US"});
        SAME(texts.said(title), std::string{"Trials"});
    }

    desk.put("/program/" + std::string(36, 'a') + ".json", polish);
    desk.put("/program/1pl.json", polish);
    desk.put("/program/-pl.json", polish);

    for (const std::string& code : std::vector<std::string>{std::string(36, 'a'), "1pl", "-pl"}) {
        desk.read(texts, code);
        SAME(texts.code(), std::string{"en-US"});
    }

    desk.put("/program/a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-p-q-r.json", polish);
    desk.read(texts, "a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-p-q-r");
    SAME(texts.code(), std::string{"a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-p-q-r"});
}

TRIAL("texts: the user's folder beats the program's, and English can be read over itself") {
    Desk desk;
    Texts texts = made();

    desk.put("/program/pl-PL.json", polish);
    desk.put("/user/pl-PL.json", R"({"title":"Moje próby"})");
    desk.read(texts, "pl-PL");
    SAME(texts.code(), std::string{"pl-PL"});
    SAME(texts.said(title), std::string{"Moje próby"});
    SAME(texts.said(quit), std::string{"Quit"});

    desk.put("/user/pl-PL.json", R"({"title":)");
    desk.read(texts, "pl-PL");
    SAME(texts.code(), std::string{"en-US"});

    desk.put("/user/en-US.json", R"({"title":"Better trials"})");
    desk.read(texts, "en-US");
    SAME(texts.code(), std::string{"en-US"});
    SAME(texts.said(title), std::string{"Better trials"});
    SAME(texts.said(quit), std::string{"Quit"});
}

TRIAL("texts: a translation from outside stands only where it fits; English stands for the rest") {
    Desk desk;
    Texts texts = made();

    desk.put("/user/pl-PL.json", R"({"meta":{"name":7},"title":"Próby {}","menu":{"left":{"text":"Lewy","key":"Le"},"quit":["Wyjście"]},)"
                                 R"("panel":{"marked":{"one":"{} plik","few":"{} pliki"},"moved":"Przeniesiono {} do {}"},"extra":"x"})");
    desk.read(texts, "pl-PL");

    SAME(texts.code(), std::string{"pl-PL"});
    SAME(texts.said(name), std::string{"English"});
    SAME(texts.said(title), std::string{"Trials"});
    SAME(texts.said(left), std::string{"Lewy"});
    SAME(texts.letter(key), U'L');
    SAME(texts.said(quit), std::string{"Quit"});
    SAME(texts.counted(marked, 5), std::string{"{} files marked"});
    SAME(texts.counted(marked, 1), std::string{"{} file marked"});
    SAME(texts.said(moved), std::string{"Przeniesiono {} do {}"});

    desk.put("/user/pl-PL.json", R"({"menu":"Menu","panel":{"marked":{"one":"{} plik","few":"{} pliki","many":"{} plików","other":"{} pliku","x":1},)"
                                 R"("moved":"Przeniesiono {}"}})");
    desk.read(texts, "pl-PL");

    SAME(texts.said(left), std::string{"Left"});
    SAME(texts.letter(key), U'L');
    SAME(texts.counted(marked, 1), std::string{"{} plik"});
    SAME(texts.counted(marked, 2), std::string{"{} pliki"});
    SAME(texts.counted(marked, 5), std::string{"{} plików"});
    SAME(texts.said(moved), std::string{"Moved {} to {}"});

    desk.put("/user/pl-PL.json", R"({"panel":{"marked":{"one":"{} plik {}","few":"{} pliki","many":"{} plików"},"moved":"{} {} {}"}})");
    desk.read(texts, "pl-PL");

    SAME(texts.counted(marked, 2), std::string{"{} files marked"});
    SAME(texts.said(moved), std::string{"Moved {} to {}"});

    desk.put("/user/pl-PL.json", R"({"panel":{"marked":{"one":"{} plik","few":7,"many":"{} plików"}},"menu":{"left":{"key":1}}})");
    desk.read(texts, "pl-PL");

    SAME(texts.counted(marked, 1), std::string{"{} file marked"});
    SAME(texts.letter(key), U'L');

    desk.put("/user/pl-PL.json", R"({"panel":{"marked":"{} plików"},"menu":{"left":{"key":""}}})");
    desk.read(texts, "pl-PL");

    SAME(texts.counted(marked, 1), std::string{"{} file marked"});
    SAME(texts.letter(key), U'L');
}

TRIAL("texts: a text for a number takes the form of the language it came from") {
    Desk desk;
    Texts texts = made();

    desk.put("/user/ja-JP.json", R"({"panel":{"marked":{"other":"{} 個"}}})");
    desk.read(texts, "ja-JP");

    SAME(texts.counted(marked, 1), std::string{"{} 個"});
    SAME(texts.counted(marked, 5), std::string{"{} 個"});

    desk.put("/user/ru-RU.json", R"({"panel":{"marked":{"one":"{} файл","few":"{} файла","many":"{} файлов"}}})");
    desk.read(texts, "ru-RU");

    SAME(texts.counted(marked, 21), std::string{"{} файл"});
    SAME(texts.counted(marked, 11), std::string{"{} файлов"});
    SAME(texts.counted(marked, 24), std::string{"{} файла"});

    desk.put("/user/fr-FR.json", R"({"panel":{"marked":{"one":"{} fichier","many":"{} de fichiers","other":"{} fichiers"}}})");
    desk.read(texts, "fr-FR");

    SAME(texts.counted(marked, 0), std::string{"{} fichier"});
    SAME(texts.counted(marked, 2), std::string{"{} fichiers"});
    SAME(texts.counted(marked, 2'000'000), std::string{"{} de fichiers"});
}

TRIAL("texts: auto is the system's language among the files, or English") {
    Desk desk;
    Texts texts = made();

    desk.put("/program/pl-PL.json", polish);

#ifndef _WIN32
    const char* before = std::getenv("LC_ALL");
    const std::optional<std::string> kept = before != nullptr ? std::optional<std::string>{before} : std::nullopt;

    ::setenv("LC_ALL", "pl_PL.UTF-8", 1);
    desk.read(texts, "auto");
    SAME(texts.code(), std::string{"pl-PL"});

    ::setenv("LC_ALL", "pl", 1);
    desk.read(texts, "auto");
    SAME(texts.code(), std::string{"pl-PL"});

    ::setenv("LC_ALL", "de_DE.UTF-8", 1);
    desk.read(texts, "auto");
    SAME(texts.code(), std::string{"en-US"});

    ::setenv("LC_ALL", "C", 1);
    desk.read(texts, "pl-PL");
    desk.read(texts, "auto");
    SAME(texts.code(), std::string{"en-US"});

    if (kept) ::setenv("LC_ALL", kept->c_str(), 1);
    else ::unsetenv("LC_ALL");
#endif

    const std::optional<tal::text::Language> system = tal::text::proposed();
    const std::string found = system ? Texts::nearest(system->tag(), {"en-US", "pl-PL"}) : std::string{};

    desk.read(texts, "auto");
    SAME(texts.code(), found.empty() ? std::string{"en-US"} : found);
}

// A disk given to the dictionary is read instead of the default one, which keeps its own files;
// with none given, the default one is read again.
TRIAL("texts: a port given is read instead of the default one") {
    Desk desk;
    Texts texts = made();
    const auto own = std::make_shared<tal::fs::Memory>();

    tal::wait(desk.loop, own->make(Path{"/program"}));
    tal::wait(desk.loop, own->write(Path{"/program/pl-PL.json"}, tal::bytes(polish)));
    desk.put("/program/pl-PL.json", R"({"title":"Domyślne próby"})");

    tal::wait(desk.loop, texts.read({Path{"/program"}}, "pl-PL", own));
    SAME(texts.code(), std::string{"pl-PL"});
    SAME(texts.said(title), std::string{"Próby"});

    const std::vector<tal::Spoken> spoken = tal::wait(desk.loop, texts.languages({Path{"/program"}}, own));

    SAME(spoken.size(), std::size_t{2});
    SAME(spoken.back().name, std::string{"Polski"});
    SAME(tal::wait(desk.loop, texts.languages({Path{"/program"}})).back().name, std::string{});

    tal::wait(desk.loop, texts.read({Path{"/program"}}, "pl-PL"));
    SAME(texts.said(title), std::string{"Domyślne próby"});
}

TRIAL("texts: the languages are English first, then the files by code, each with its own name") {
    Desk desk;
    Texts texts = made();

    desk.put("/program/pl-PL.json", polish);
    desk.put("/program/de-DE.json", R"({"meta":{"name":7},"title":"Versuche"})");
    desk.put("/program/cs-CZ.json", R"({"meta":)");
    desk.put("/program/en-US.json", R"({"meta":{"name":"Other English"}})");
    desk.put("/program/notes.txt", "x");
    desk.put("/program/bad name.json", polish);
    tal::wait(desk.loop, tal::fs::make(Path{"/program/fr-FR.json"}));
    desk.put("/user/pl-PL.json", R"({"meta":{"name":"Mój polski"}})");
    desk.put("/user/uk-UA.json", R"({"meta":{"name":"Українська"}})");

    const std::vector<tal::Spoken> spoken = tal::wait(desk.loop, texts.languages({Path{"/user"}, Path{"/program"}, Path{"/nowhere"}}));
    const std::vector<tal::Spoken> wanted{{"en-US", "English"}, {"cs-CZ", ""}, {"de-DE", ""}, {"pl-PL", "Mój polski"}, {"uk-UA", "Українська"}};

    SAME(spoken.size(), wanted.size());

    for (std::size_t at = 0; at < std::min(spoken.size(), wanted.size()); ++at) {
        SAME(spoken[at].code, wanted[at].code);
        SAME(spoken[at].name, wanted[at].name);
    }

    SAME(tal::wait(desk.loop, texts.languages({})).size(), std::size_t{1});
}

TRIAL("texts: nearest finds the same tag, then the same language, in the order of the list") {
    const std::vector<std::string> known{"en-GB", "en-US", "pl-PL", "pt-BR", "pt-PT"};

    SAME(Texts::nearest("en-US", known), std::string{"en-US"});
    SAME(Texts::nearest("EN-us", known), std::string{"en-US"});
    SAME(Texts::nearest("en-AU", known), std::string{"en-GB"});
    SAME(Texts::nearest("pl", known), std::string{"pl-PL"});
    SAME(Texts::nearest("PT-pt", known), std::string{"pt-PT"});
    SAME(Texts::nearest("pt", known), std::string{"pt-BR"});
    SAME(Texts::nearest("de-DE", known), std::string{});
    SAME(Texts::nearest("", known), std::string{});
    SAME(Texts::nearest("pl", {}), std::string{});
}

TRIAL("texts: said, counted and letter refuse the wrong kind of text; filled fills from the left") {
    const Texts texts = made();

    THREW((void)texts.said(marked));
    THREW((void)texts.counted(title, 1));
    THREW((void)texts.letter(left));
    THREW((void)texts.said(std::size(keys)));
    SAME(texts.letter(key), U'L');
    SAME(texts.said(name), std::string{"English"});

    SAME(tal::filled("Moved {} to {}", {"a", "b"}), std::string{"Moved a to b"});
    SAME(tal::filled("Moved {} to {}", {"a"}), std::string{"Moved a to {}"});
    SAME(tal::filled("Moved {} to {}", {"a", "b", "c"}), std::string{"Moved a to b"});
    SAME(tal::filled("Moved {} to {}", {"{}", "b"}), std::string{"Moved {} to b"});
    SAME(tal::filled("{}{}", {"x", ""}), std::string{"x"});
    SAME(tal::filled("{edition-…}", {"x"}), std::string{"{edition-…}"});
    SAME(tal::filled("", {}), std::string{});
}

TRIAL("texts: English that does not match its keys is a mistake of the program") {
    constexpr std::string_view wrong[] = {"meta.name", "title", "menu.left.text", "menu.left.key", "menu.quit", "panel.marked", "panel.move"};
    constexpr std::string_view named[] = {"meta.name"};
    constexpr std::string_view counted[] = {"meta.name", "a"};

    THREW(Texts(std::span<const std::string_view>{keys}.first(6), english));
    THREW(Texts(wrong, english));
    THREW(Texts(named, R"({"meta":{"name":""}})"));
    THREW(Texts(counted, R"({"meta":{"name":"English"},"a":{"few":"x"}})"));
    THREW(Texts(named, R"({"meta":)"));
    THREW(Texts(named, "[]"));

    const Texts one{named, R"({"meta":{"name":"English"}})"};

    SAME(one.said(0), std::string{"English"});
}

TRIAL("texts: the generator refuses names that cannot be C++") {
    const std::string start = R"({"meta":{"name":"English"},)";

    SAME(alone(generator(start + R"("Upper":"x"})")), std::string{R"(en-US.json:1:36: "Upper": a name is small letters, digits, _ and -, from a letter)"});
    SAME(alone(generator(start + R"("9lives":"x"})")), std::string{R"(en-US.json:1:37: "9lives": a name is small letters, digits, _ and -, from a letter)"});
    SAME(alone(generator(start + R"("a b":{"c":"x"}})")), std::string{R"(en-US.json:1:34: "a b": a name is small letters, digits, _ and -, from a letter)"});
    SAME(alone(generator(start + R"("a.b":"x"})")), std::string{R"(en-US.json:1:34: "a.b": a name is small letters, digits, _ and -, from a letter)"});
    SAME(alone(generator(start + R"("a-_b":"x"})")), std::string{R"(en-US.json:1:35: "a-_b": the name becomes a__b, which C++ keeps for itself)"});
    SAME(alone(generator(start + R"("a-b":"x","a_b":"y"})")), std::string{R"(en-US.json:1:44: "a-b" and "a_b" are both a_b in C++)"});
    SAME(alone(generator(start + R"("m":{"new":"x","new_":"y"}})")), std::string{R"(en-US.json:1:50: "m.new" and "m.new_" are both m::new_ in C++)"});
    SAME(alone(generator(start + R"("dictionary":"x"})")), std::string{R"(en-US.json:1:41: "dictionary" would hide spoken::dictionary())"});
    OUGHT(generator(start + R"("m":{"dictionary":"x"}})").empty());
    SAME(alone(generator(start + R"("t":"{}{}{}{}{}{}{}{}{}{}"})")), std::string{R"(en-US.json:1:32: "t" takes more than 9 values)"});
    OUGHT(generator(start + R"("t":"{}{}{}{}{}{}{}{}{}"})").empty());

    for (const std::string_view space : {"std", "tal", "tal::x", "a::", "::a", "1a", "_a", "a__b", "delete", "a-b", ""})
        SAME(alone(generator(start + R"("t":"x"})", space)), "tal-texts: \"" + std::string{space} + "\" cannot be the namespace of the texts");

    OUGHT(generator(start + R"("t":"x"})", "my::texts").empty());
}

TRIAL("texts: the generator checks the shape of English") {
    SAME(alone(generator(R"({"meta":{"name":"English"},"a":7})")), std::string{R"(en-US.json:1:32: "a" is neither a text nor an object)"});
    SAME(alone(generator(R"({"meta":{"name":"English"},"a":{}})")), std::string{R"(en-US.json:1:32: "a" holds no texts)"});
    OUGHT(generator(R"({"meta":{"name":"English"},"a":{"text":{"b":"B"},"key":"Key:"}})").empty());
    SAME(alone(generator(R"({"meta":{"name":"English"},"a":{"text":"A","key":"AB"}})")),
         std::string{R"(en-US.json:1:50: "a.key" is the letter of a key: one character)"});
    SAME(alone(generator(R"({"meta":{"name":"English"},"a":{"one":"x","few":"y","other":"z"}})")),
         std::string{R"(en-US.json:1:49: "a" has "few", which is not a plural form of en-US)"});
    SAME(alone(generator(R"({"meta":{"name":"English"},"a":{"one":"x"}})")), std::string{R"(en-US.json:1:32: "a" lacks the plural form "other")"});
    SAME(alone(generator(R"({"meta":{"name":"English"},"a":{"one":"x","other":"{} y"}})")),
         std::string{R"(en-US.json:1:32: "a" has 0 {} in the plural form "one" where en-US has 1)"});
    SAME(alone(generator(R"({"meta":{"name":"English"},"a":{"one":1,"other":"y"}})")),
         std::string{R"(en-US.json:1:32: "a" has the plural form "one" not as a text)"});
    SAME(alone(generator(R"({"meta":{"name":""}})")), std::string{R"(en-US.json:1:17: "meta.name" is empty)"});
    SAME(alone(generator(R"({"title":"x"})")), std::string{R"(en-US.json:1:1: "meta.name" is missing)"});
    SAME(alone(generator(R"({"meta":{"name":{"x":"y"}}})")), std::string{R"(en-US.json:1:17: "meta.name" is not a text)"});
    SAME(alone(generator(R"(["x"])")), std::string{R"(en-US.json:1:1: the texts are not an object)"});
    SAME(alone(generator("{\n  \"meta\": {\"name\": \"English\"},\n  \"a\": \"x\",\n}")), std::string{"en-US.json:4:1: a name in quotes is expected"});
    SAME(alone(generator(R"({"a":1,"a":2})")), std::string{"en-US.json:1:8: a name is repeated"});

    const tal::generating::Made made = [] {
        std::vector<std::string> problems;

        return tal::generating::generated({"en-US.json", std::string{english}}, "my::texts", "texts.hpp", problems);
    }();

    std::vector<std::string> problems;

    OUGHT(tal::generating::generated({"en-US.json", R"({"meta":{"name":"English"},"T":"x"})"}, "my::texts", "texts.hpp", problems).header.empty());
    SAME(problems.size(), std::size_t{1});

    OUGHT(made.header.find("namespace my::texts {\n\n[[nodiscard]] ::tal::Texts& dictionary();\n\n}\n") != std::string::npos);
    OUGHT(made.header.find("namespace my::texts::panel {\n\n[[nodiscard]] ::std::string marked(::std::uint64_t many, ::std::string_view first);\n"
                           "[[nodiscard]] ::std::string moved(::std::string_view first, ::std::string_view second);\n") != std::string::npos);
    OUGHT(made.header.find("[[nodiscard]] char32_t key();") != std::string::npos);
    OUGHT(made.code.starts_with("#include \"texts.hpp\"\n"));
    OUGHT(made.code.find("{ return ::tal::filled(::my::texts::dictionary().counted(5, many), {first}); }") != std::string::npos);
    OUGHT(made.code.find("{ return ::my::texts::dictionary().letter(3); }") != std::string::npos);
}

TRIAL("texts: the check of a language beside English") {
    OUGHT(checked("pl-PL.json", polish).empty());
    OUGHT(checked("/a/b/pl-PL.json", polish).empty());
    OUGHT(checked("C:\\a\\pl-PL.json", polish).empty());

    OUGHT(checked("pl.json", polish).empty());
    SAME(alone(checked("pl_PL.json", polish)), std::string{"pl_PL.json: the name of the file is not the code of a language"});
    SAME(alone(checked("pl-PL.json", R"({"meta":)")), std::string{"pl-PL.json:1:9: a value is missing"});

    std::string given{polish};

    const auto changed = [&given](std::string_view from, std::string_view to) {
        std::string made = given;

        made.replace(made.find(from), from.size(), to);

        return made;
    };

    SAME(alone(checked("pl-PL.json", changed(R"("quit":"Wyjście")", R"("quitt":"Wyjście")"))),
         std::string{"2 problems:\npl-PL.json:1:50: \"menu.quit\" is missing\npl-PL.json:1:92: \"menu.quitt\" is not in en-US"});
    SAME(alone(checked("pl-PL.json", changed(R"("moved":"Przeniesiono {} do {}")", R"("moved":"Przeniesiono {}")"))),
         std::string{R"(pl-PL.json:1:182: "panel.moved" has 1 {} where en-US has 2)"});
    SAME(alone(checked("pl-PL.json", changed(R"("key":"Ł")", R"("key":"Łó")"))),
         std::string{R"(pl-PL.json:1:79: "menu.left.key" is the letter of a key: one character)"});
    SAME(alone(checked("pl-PL.json", changed(R"("quit":"Wyjście")", R"("quit":{"text":"Wyjście"})"))),
         std::string{R"(pl-PL.json:1:91: "menu.quit" is not a text)"});
    SAME(alone(checked("pl-PL.json", changed(R"("left":{"text":"Lewy","key":"Ł"})", R"("left":"Lewy")"))),
         std::string{R"(pl-PL.json:1:58: "menu.left" is not an object)"});
    SAME(alone(checked("pl-PL.json", changed(R"({"one":"{} plik","few":"{} pliki","many":"{} plików"})", R"("{} plików")"))),
         std::string{R"(pl-PL.json:1:120: "panel.marked" is not an object of plural forms)"});
    SAME(alone(checked("pl-PL.json", changed(R"(,"few":"{} pliki")", ""))), std::string{R"(pl-PL.json:1:120: "panel.marked" lacks the plural form "few")"});
    SAME(alone(checked("pl-PL.json", changed(R"("one":"{} plik",)", R"("one":"{} plik","other":"{} pliku",)"))),
         std::string{R"(pl-PL.json:1:145: "panel.marked" has "other", which is not a plural form of pl-PL)"});
    SAME(alone(checked("pl-PL.json", changed(R"("name":"Polski")", R"("name":"")"))), std::string{R"(pl-PL.json:1:17: "meta.name" is empty)"});
    SAME(alone(checked("pl-PL.json", "[]")), std::string{"pl-PL.json:1:1: the texts are not an object"});
    SAME(alone(tal::generating::checked({"en-US.json", R"({"meta":{"name":""}})"}, {{"pl-PL.json", "[]"}})),
         std::string{R"(en-US.json:1:17: "meta.name" is empty)"});
}

TRIAL("texts: no damage to a translation makes said, counted or letter throw or change the values") {
    Desk desk;
    Texts texts = made();
    std::mt19937 random{536};
    const Texts plain = made();

    for (int round = 0; round < 3000; ++round) {
        std::string damaged{polish};
        const int much = 1 + static_cast<int>(random() % 4);

        for (int step = 0; step < much && !damaged.empty(); ++step) {
            const std::size_t at = random() % damaged.size();
            const char pieces[] = {'{', '}', '"', ',', ':', '[', ']', '1', 'x', '\\', '\xC5'};

            switch (random() % 3) {
                case 0: damaged.erase(at, 1 + random() % 6); break;
                case 1: damaged.insert(at, 1, pieces[random() % sizeof pieces]); break;
                default: damaged.insert(at, "{}"); break;
            }
        }

        desk.put("/user/pl-PL.json", damaged);
        desk.read(texts, "pl-PL");

        for (std::size_t at = 0; at < std::size(keys); ++at) {
            if (at == key) {
                const char32_t letter = texts.letter(at);

                OUGHT(letter != 0);
            } else if (at == marked) {
                for (const std::uint64_t many : {0, 1, 2, 5, 22, 1'000'000}) SAME(values(texts.counted(at, many)), values(plain.counted(at, many)));
            } else {
                SAME(values(texts.said(at)), values(plain.said(at)));
            }
        }
    }
}
