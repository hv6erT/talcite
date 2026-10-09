#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "proving.hpp"

// Spójność CLAUDE.md z kodem: każda nazwa ujęta w opisie w odwrotne apostrofy ma naprawdę
// istnieć. Opis mówiący o rzeczach nieistniejących wygląda na prawdę i dlatego jest gorszy niż
// brak opisu.
//
// **Nazwa jest sprawdzana wobec dokumentu, który ją definiuje**, a nie wobec sklejki całego
// tekstu projektu. Rodziny i ich źródła prawdy:
//
//   nazwa C++     — źródła biblioteki, prób i pokazów
//   rola skórki   — napis w źródłach, bo rola nazywa się tam, gdzie się ją nadaje
//   nazwa pliku   — wykaz plików projektu
//
// Zapisy z kropkami, które nie są ani rolą, ani plikiem, to wyrażenia (`kept_.clear`, `seen.size`),
// a nie nazwy — biblioteka nie ma dokumentów kluczowanych, więc nie ma czego z nimi zestawiać.
//
// **Nazwy cudzej i odrzuconej nie ubieramy w odwrotne apostrofy.** Powód, dla którego czegoś
// u nas nie ma, należy do zdania, które o tym mówi, a nie do listy wyjątków w próbie — lista
// wyjątków jest kryjówką. Przedtem stała tu taka lista na pięćdziesiąt wpisów i to w niej
// przeżyły nazwy dawno zniknięte z API. Dlatego listy nie ma wcale: OpenSSL, Win32, POSIX, klucze
// skórek Midnight Commandera, pomysły odrzucone i rzeczy jeszcze nienapisane idą gołe.
//
// Próba **nie sprawdza samej siebie**: własnego pliku nie wciąga do stogu. Inaczej każda nazwa
// w niej wypisana dowodziłaby swojego istnienia.
//
// Jedno miejsce zostaje słabsze świadomie: **nazwa C++ bez kwalifikatora** szukana jest w tekście
// źródeł razem z komentarzami, bo komentarze są jedynym zaczepieniem cudzego słownictwa, które
// opis wymienia z sensem. Nazwa z kwalifikatorem (`Page::hold`) sprawdzana jest mocniej: musi stać
// w źródłach dosłownie albo w ciele swojej klasy — także odziedziczonym.

namespace {

namespace stood = std::filesystem;

// Korzeń projektu liczony od tego pliku: `test/naming.cpp` leży o jeden katalog niżej.
stood::path root() { return stood::path{__FILE__}.parent_path().parent_path(); }

std::string slurped(const stood::path& where) {
    std::ifstream input{where, std::ios::binary};

    std::ostringstream held;

    held << input.rdbuf();

    return held.str();
}

bool ends(std::string_view whole, std::string_view tail) {
    return whole.size() >= tail.size() &&
           whole.compare(whole.size() - tail.size(), tail.size(), tail) == 0;
}

bool coded(const stood::path& one) {
    const std::string ending = one.extension().string();

    return ending == ".hpp" || ending == ".cpp";
}

// Tekst źródeł. Własny plik zostaje poza stogiem — patrz uwaga na górze.
std::string gathered(const stood::path& from) {
    std::string whole;

    if (!stood::exists(from)) return whole;

    for (const auto& one : stood::recursive_directory_iterator{from})
        if (one.is_regular_file() && coded(one.path()) && one.path().filename() != "naming.cpp")
            whole += slurped(one.path()) + '\n';

    return whole;
}

bool wordy(char one) { return std::isalnum(static_cast<unsigned char>(one)) != 0 || one == '_'; }

// Wystąpienie całym słowem, a nie kawałkiem innego: `put` nie może zaliczyć się przez `output`.
bool present(std::string_view whole, std::string_view stem) {
    if (stem.empty()) return false;

    for (std::size_t at = whole.find(stem); at != std::string_view::npos;
         at = whole.find(stem, at + 1)) {
        const bool before = at == 0 || !wordy(whole[at - 1]);
        const bool after = at + stem.size() == whole.size() || !wordy(whole[at + stem.size()]);

        if (before && after) return true;
    }

    return false;
}

// Ciało klasy albo struktury o podanej nazwie i wykaz jej podstaw. Szukamy słowa `class` albo
// `struct` **przed** nazwą, żeby nie wziąć użycia za deklarację, i przeskakujemy zapowiedzi
// (`class Node;`), bo one ciała nie mają.
struct Told {
    std::string body;
    std::vector<std::string> above;
};

std::vector<Told> told(std::string_view whole, std::string_view name) {
    std::vector<Told> every;

    for (std::size_t at = whole.find(name); at != std::string_view::npos;
         at = whole.find(name, at + 1)) {
        if (at + name.size() < whole.size() && wordy(whole[at + name.size()])) continue;

        std::size_t back = at;

        while (back > 0 && (whole[back - 1] == ' ' || whole[back - 1] == '\t')) --back;

        const bool klass = back >= 5 && whole.substr(back - 5, 5) == "class";
        const bool structure = back >= 6 && whole.substr(back - 6, 6) == "struct";

        if (!klass && !structure) continue;

        Told made;

        std::size_t here = at + name.size();
        std::string bases;

        while (here < whole.size() && whole[here] != '{' && whole[here] != ';') {
            bases += whole[here];
            ++here;
        }

        if (here == whole.size() || whole[here] == ';') continue;

        std::size_t deep = 0;
        const std::size_t from = here;

        for (; here < whole.size(); ++here) {
            if (whole[here] == '{') ++deep;
            else if (whole[here] == '}' && --deep == 0) break;
        }

        made.body = std::string{whole.substr(from, here - from)};

        std::string word;

        for (const char one : bases) {
            if (wordy(one)) {
                word += one;
            } else {
                if (word.size() > 2 && word != "public" && word != "private" &&
                    word != "protected" && word != "virtual")
                    made.above.push_back(word);

                word.clear();
            }
        }

        if (!word.empty() && word != "public" && word != "private" && word != "protected")
            made.above.push_back(word);

        every.push_back(std::move(made));
    }

    return every;
}

struct Named {
    std::set<std::string> files;
    std::string code;
    std::string build;
};

Named taken() {
    Named made;

    const stood::path here = root();

    for (const char* base : {"talcite", "test", "demo"}) made.code += gathered(here / base);

    for (const char* one : {"CMakeLists.txt", "CMakePresets.json", "talcite/CMakeLists.txt",
                            "test/CMakeLists.txt", "demo/CMakeLists.txt"})
        made.build += slurped(here / one) + '\n';

    for (const auto& one : stood::directory_iterator{here})
        if (one.is_regular_file()) made.files.insert(one.path().filename().string());

    for (const char* base : {"talcite", "test", "demo"})
        for (const auto& one : stood::recursive_directory_iterator{here / base})
            if (one.is_regular_file()) made.files.insert(one.path().filename().string());

    return made;
}

// Nazwa z kwalifikatorem: albo stoi w źródłach dosłownie (`void Page::hold(...)`), albo jej człon
// leży w ciele klasy — własnym lub odziedziczonym, bo `Select::options` przychodzi z `OptionsNode`.
bool inherited(const Named& all, const std::string& owner, const std::string& stem, int deep) {
    if (deep > 4) return false;

    for (const Told& one : told(all.code, owner)) {
        if (present(one.body, stem)) return true;

        for (const std::string& up : one.above)
            if (inherited(all, up, stem, deep + 1)) return true;
    }

    return false;
}

bool identifier(std::string_view one) {
    if (one.empty() || (!std::isalpha(static_cast<unsigned char>(one[0])) && one[0] != '_'))
        return false;

    return std::all_of(one.begin(), one.end(), wordy);
}

// `Node` albo `Page::hold`; `nazwa()` liczy się tak samo, jak `nazwa`.
bool cplusplus(std::string_view one, std::string& owner, std::string& stem) {
    const std::size_t colon = one.find("::");

    if (colon == std::string_view::npos) {
        if (!identifier(one)) return false;

        owner.clear();
        stem = std::string{one};

        return true;
    }

    if (one.find("::", colon + 2) != std::string_view::npos) return false;

    if (!identifier(one.substr(0, colon)) || !identifier(one.substr(colon + 2))) return false;

    owner = std::string{one.substr(0, colon)};
    stem = std::string{one.substr(colon + 2)};

    return true;
}

// `talcite.frame`, `node.cpp` — człony rozdzielone kropkami, bez znaków, które robią z zapisu
// wzorzec (`talcite.panel.name.*`) albo kawałek kodu.
bool dotted(std::string_view one) {
    if (one.empty() || one.find('.') == std::string_view::npos) return false;

    bool fresh = true;

    for (const char letter : one) {
        if (letter == '.') {
            if (fresh) return false;

            fresh = true;

            continue;
        }

        if (!wordy(letter)) return false;

        fresh = false;
    }

    return !fresh;
}

bool filed(std::string_view one) {
    for (const char* tail : {".cpp", ".hpp", ".json", ".md", ".txt", ".ini", ".c", ".h"})
        if (ends(one, tail)) return true;

    return false;
}

}

TRIAL("Nazwy: każda nazwa z CLAUDE.md istnieje naprawdę") {
    const std::string doc = slurped(root() / "CLAUDE.md");

    ought(!doc.empty());

    const Named all = taken();

    std::vector<std::string> bad;

    std::size_t line = 1;

    for (std::size_t at = 0; at < doc.size(); ++at) {
        if (doc[at] == '\n') {
            ++line;

            continue;
        }

        if (doc[at] != '`') continue;

        const std::size_t upto = doc.find_first_of("`\n", at + 1);

        if (upto == std::string::npos || doc[upto] == '\n') continue;

        std::string one = doc.substr(at + 1, upto - at - 1);

        at = upto;

        if (one.size() > 2 && one.compare(one.size() - 2, 2, "()") == 0) one.resize(one.size() - 2);

        const std::string where = "  wiersz " + std::to_string(line) + ": " + one + " — ";

        std::string owner;
        std::string stem;

        if (dotted(one)) {
            if (filed(one)) {
                if (!all.files.contains(one)) bad.push_back(where + "nie ma takiego pliku");
            } else if (one.rfind("talcite.", 0) == 0) {
                if (!present(all.code, '"' + one + '"'))
                    bad.push_back(where + "nie ma takiej roli w źródłach");
            }
        } else if (cplusplus(one, owner, stem)) {
            if (!owner.empty()) {
                if (present(all.code, one)) continue;

                if (inherited(all, owner, stem, 0)) continue;

                bad.push_back(where + "nie ma jej w źródłach ani w ciele " + owner);
            } else {
                if (stem.size() < 3) continue;

                if (present(all.code, stem) || present(all.build, stem)) continue;

                bad.push_back(where + "nie ma jej w źródłach biblioteki");
            }
        }
    }

    if (!bad.empty()) {
        std::string said = "nazwy z CLAUDE.md, których nie ma:";

        for (const std::string& one : bad) said += '\n' + one;

        said += "\n\n  Nazwa cudza albo odrzucona nie idzie w odwrotnych apostrofach.";

        ::proving::fell(__FILE__, __LINE__, said);
    }
}

// **Nagłówek publiczny jest goły** — to reguła projektu: omówienie żyje w `.cpp` i w CLAUDE.md,
// a nazwa w nagłówku ma mówić sama za siebie. Reguła stała na papierze i trzy nagłówki ją łamały,
// zanim ktoś to zauważył; ta próba pilnuje jej tak samo jak nazw — pokazuje plik i wiersz.
// Schodzi też do `detail/`: wnętrzności obietnicy są widoczne dla każdego, kto dołącza
// `promise.hpp`, więc reguła obowiązuje je tak samo.
TRIAL("Nazwy: nagłówki publiczne nie mają komentarzy") {
    std::vector<std::string> bad;

    for (const stood::directory_entry& one :
         stood::recursive_directory_iterator{root() / "talcite" / "include" / "talcite"}) {
        if (one.path().extension() != ".hpp") continue;

        std::istringstream lines{slurped(one.path())};
        std::string line;
        std::size_t number = 0;

        while (std::getline(lines, line)) {
            ++number;

            const std::size_t first = line.find_first_not_of(" \t");

            if (first == std::string::npos) continue;

            const std::string_view rest = std::string_view{line}.substr(first);

            if (rest.starts_with("//") || rest.starts_with("/*"))
                bad.push_back("  " + one.path().filename().string() + ":" + std::to_string(number));
        }
    }

    if (!bad.empty()) {
        std::string said = "komentarze w nagłówkach publicznych:";

        for (const std::string& one : bad) said += '\n' + one;

        ::proving::fell(__FILE__, __LINE__, said);
    }
}
