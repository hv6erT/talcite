#include "proving.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "talcite/collate.hpp"

using talcite::Collation;

namespace {

// The words sorted by the collation, through their keys, as a sort uses it.
std::vector<std::string> ordered(const Collation& by, std::vector<std::string> words) {
    std::sort(words.begin(), words.end(), [&by](const std::string& one, const std::string& two) {
        return by.key(one) < by.key(two);
    });

    return words;
}

}

// The common order: a letter with marks stands with its base and after it when the rest is equal,
// lowercase before uppercase, spaces and punctuation before digits, digits before letters.
TRIAL("Collation: marks and case come after the letter, punctuation and digits before letters") {
    const Collation common;

    same(ordered(common, {"Zebra", "éclair", "eclair", "Eclair", "apple", "Äpfel", "2001", "-x", "zoo"}),
         (std::vector<std::string>{"-x", "2001", "Äpfel", "apple", "eclair", "Eclair", "éclair", "Zebra", "zoo"}));

    ought(common.compare("a", "b") < 0);
    ought(common.compare("b", "a") > 0);
    same(common.compare("same", "same"), 0);

    // A shorter text that begins the longer one comes first.
    ought(common.compare("Alien", "Aliens") < 0);
}

// Polish puts its letters after their bases: a < ą < b, l < ł < m, z < ź < ż, and ó after o.
TRIAL("Collation: Polish letters stand after their bases") {
    const Collation polish{"pl-PL"};

    same(ordered(polish, {"żaba", "zebra", "źrebię", "łoś", "lis", "mysz", "ąb", "b", "a", "ósmy", "ozon", "pies"}),
         (std::vector<std::string>{"a", "ąb", "b", "lis", "łoś", "mysz", "ozon", "ósmy", "pies", "zebra", "źrebię", "żaba"}));

    // Uppercase letters keep the same places.
    same(ordered(polish, {"Żółw", "Zorro", "Łódź", "Lublin"}), (std::vector<std::string>{"Lublin", "Łódź", "Zorro", "Żółw"}));

    // Without the tailoring ą is a with a mark and stands before b, as in German.
    const Collation common{"de-DE"};

    same(ordered(common, {"az", "ąb"}), (std::vector<std::string>{"ąb", "az"}));
    same(ordered(polish, {"az", "ąb"}), (std::vector<std::string>{"az", "ąb"}));
}

// Czech and Slovak read ch as one letter after h, and č, ř, š, ž as letters after their bases.
TRIAL("Collation: Czech ch comes after h, and č after c") {
    const Collation czech{"cs"};

    same(ordered(czech, {"chata", "hrad", "cibule", "čaj", "dům", "ivan", "Chrudim"}),
         (std::vector<std::string>{"cibule", "čaj", "dům", "hrad", "chata", "Chrudim", "ivan"}));

    const Collation slovak{"sk-SK"};

    same(ordered(slovak, {"ôsmy", "oko", "päť", "ändern", "auto", "b"}),
         (std::vector<std::string>{"auto", "ändern", "b", "oko", "ôsmy", "päť"}));

    // In English ch is just c and h.
    same(ordered(Collation{"en"}, {"chata", "hrad", "cibule"}), (std::vector<std::string>{"chata", "cibule", "hrad"}));
}

// Spanish ñ after n; German ß as ss, æ as ae; ł and ø with their base letters in the common order.
TRIAL("Collation: ñ in Spanish, ß and æ as two letters, ł and ø by their bases") {
    same(ordered(Collation{"es-ES"}, {"ñu", "nube", "oso"}), (std::vector<std::string>{"nube", "ñu", "oso"}));
    // Elsewhere ñ is n with a mark, so the shorter word comes first.
    same(ordered(Collation{"en"}, {"ñu", "nube", "oso"}), (std::vector<std::string>{"ñu", "nube", "oso"}));

    const Collation german{"de"};

    same(ordered(german, {"Strauß", "Strausberg", "Straus", "Strauss"}),
         (std::vector<std::string>{"Straus", "Strausberg", "Strauss", "Strauß"}));
    same(ordered(german, {"Aesop", "Æneid", "Afrika"}), (std::vector<std::string>{"Æneid", "Aesop", "Afrika"}));
    same(ordered(Collation{}, {"Lyon", "Łódź", "Lima"}), (std::vector<std::string>{"Lima", "Łódź", "Lyon"}));
    same(ordered(Collation{}, {"Oslo", "Ørsted", "Osaka", "Paris"}), (std::vector<std::string>{"Ørsted", "Osaka", "Oslo", "Paris"}));
}

// Cyrillic goes by its alphabet: й after и, ё with е, and the Ukrainian і, ї, ґ, є in their places;
// Greek by its own, accents as marks; Latin before Greek before Cyrillic.
TRIAL("Collation: Cyrillic and Greek by their alphabets") {
    const Collation russian{"ru-RU"};

    same(ordered(russian, {"яблоко", "ёж", "еда", "йога", "игра", "кот", "Юла", "жук"}),
         (std::vector<std::string>{"еда", "ёж", "жук", "игра", "йога", "кот", "Юла", "яблоко"}));

    const Collation ukrainian{"uk-UA"};

    same(ordered(ukrainian, {"їжак", "іній", "йод", "ґанок", "гора", "дім", "єнот", "ехо", "иній"}),
         (std::vector<std::string>{"гора", "ґанок", "дім", "ехо", "єнот", "иній", "іній", "їжак", "йод"}));

    same(ordered(Collation{"el"}, {"ωμέγα", "άλφα", "βήτα", "αλφα"}), (std::vector<std::string>{"αλφα", "άλφα", "βήτα", "ωμέγα"}));

    same(ordered(Collation{}, {"дом", "home", "οίκος"}), (std::vector<std::string>{"home", "οίκος", "дом"}));
}

// A mark written apart, after its letter, weighs as the same letter written whole; the texts stay
// distinct, so only the bytes tell them apart, and two different texts never compare equal.
TRIAL("Collation: a separate combining mark weighs as the composed letter") {
    const Collation common;

    const std::string composed = "caf\xc3\xa9";
    const std::string apart = "cafe\xcc\x81";

    ought(common.compare(composed, apart) != 0);
    ought(common.compare("cafe", apart) < 0);
    ought(common.compare(apart, "cafes") < 0);
    ought(common.compare(composed, "cafes") < 0);

    // Hangul syllables keep the order of their code points, which is that of the alphabet.
    same(ordered(common, {"하나", "가방", "나무"}), (std::vector<std::string>{"가방", "나무", "하나"}));
}

// The language is the part of the code before the region, in any case; an unknown one is common.
TRIAL("Collation: the language code picks the tailoring, region and case aside") {
    same(Collation{"PL_pl"}.compare("łza", "mama"), Collation{"pl"}.compare("łza", "mama"));
    ought(Collation{"pl"}.compare("łza", "lza") > 0);
    ought(Collation{"xx"}.compare("ąb", "az") < 0);
    same(Collation{"pt-BR"}.language(), std::string{"pt-BR"});
}
