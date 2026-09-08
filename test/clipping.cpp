#include "proving.hpp"

#include <string>

#include "talcite/clipboard.hpp"

#ifndef _WIN32

#include <unistd.h>

#endif

using namespace talcite;

namespace {

// Kopiowanie **z zatkanym wyjściem**. Na POSIX-ie `copied` pisze OSC 52 prosto na wyjście, więc
// bez tego każda próba wysypywałaby sekwencję sterującą w wynik przebiegu — a przy okazji nie da
// się inaczej sprawdzić, czy poszło dokładnie to, co miało pójść.
struct Went {
    bool went = false;
    std::string said;
};

Went aside(const std::string& what) {
    Went made;

#ifdef _WIN32

    made.went = copied(what);

    return made;

#else

    int pair[2];

    if (pipe(pair) != 0) return made;

    const int was = dup(STDOUT_FILENO);

    (void)dup2(pair[1], STDOUT_FILENO);

    made.went = copied(what);

    (void)dup2(was, STDOUT_FILENO);

    close(was);
    close(pair[1]);

    char room[4096];

    for (;;) {
        const ssize_t got = read(pair[0], room, sizeof(room));

        if (got <= 0) break;

        made.said.append(room, static_cast<std::size_t>(got));

        if (static_cast<std::size_t>(got) < sizeof(room)) break;
    }

    close(pair[0]);

    return made;

#endif
}

std::string spouted(const std::string& what) { return aside(what).said; }

bool kept(const std::string& what) { return aside(what).went; }

}

// --- Co pamięta program --------------------------------------------------------------------------

TRIAL("Clipboard: skopiowane wraca przy wklejaniu") {
    ought(kept("ala ma kota"));

    const std::optional<std::string> got = pasted();

    ought(got.has_value());
    same(*got, std::string{"ala ma kota"});
}

TRIAL("Clipboard: pustka nie jest treścią") {
    ought(kept(""));
    ought(!pasted().has_value());
}

// Próg dotyczy **wyłącznie drogi przez terminal**: OSC 52 bywa po cichu ucinane, więc lepiej
// powiedzieć „nie poszło" niż wstawić ogryzek udający całość. Schowek systemowy Windows żadnego
// takiego progu nie ma i bierze całość — dlatego zdanie o odmowie stawiamy tylko tam, gdzie droga
// przez terminal jest jedyną drogą.
//
// Wspólne dla obu jest to, po co ten próg w ogóle istnieje: cokolwiek powie schowek systemu,
// **kopia zostaje u nas pełna** i wraca w całości.
TRIAL("Clipboard: treść ponad próg zostaje u nas w całości") {
    const std::string big(200 * 1024, 'x');

    const bool went = kept(big);

#ifdef _WIN32

    (void)went;

#else

    ought(!went);

#endif

    const std::optional<std::string> got = pasted();

    ought(got.has_value());
    same(got->size(), big.size());
}

TRIAL("Clipboard: znaki wielobajtowe przechodzą bez zmian") {
    const std::string what = "z\xc4\x85\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87";

    ought(kept(what));
    same(*pasted(), what);
}

// --- Co idzie do terminala -----------------------------------------------------------------------

#ifndef _WIN32

TRIAL("Clipboard: wychodzi OSC 52 z treścią w base64") {
    same(spouted("abc"), std::string{"\x1b]52;c;YWJj\x07"});
}

// Dopełnienie liczy się co do znaku: terminal, który dostanie base64 bez dopełnienia, potrafi
// odrzucić całość.
TRIAL("Clipboard: base64 dopełnia się na wszystkich trzech resztach") {
    same(spouted("a"), std::string{"\x1b]52;c;YQ==\x07"});
    same(spouted("ab"), std::string{"\x1b]52;c;YWI=\x07"});
    same(spouted("abc"), std::string{"\x1b]52;c;YWJj\x07"});
    same(spouted("abcd"), std::string{"\x1b]52;c;YWJjZA==\x07"});
}

TRIAL("Clipboard: base64 nie gubi bajtów spoza ASCII") {
    same(spouted("\xc4\x85"), std::string{"\x1b]52;c;xIU=\x07"});
}

TRIAL("Clipboard: ponad próg nie wychodzi nic") {
    same(spouted(std::string(200 * 1024, 'x')), std::string{});
}

#endif
