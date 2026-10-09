#include "proving.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "faking.hpp"
#include "talcite/clipboard.hpp"

using namespace talcite;

namespace {

// Schowek na atrapie terminala **przez ssh**. Na POSIX-ie droga i tak jest jedna — OSC 52 — ale
// na Windows dopiero ssh kieruje schowek do terminala. Dzięki temu te same próby idą na każdym
// systemie, a żadna nie sięga do schowka systemowego: dawniej próba na Windows nadpisywała
// użytkownikowi to, co miał skopiowane.
struct Wired {
    std::shared_ptr<faking::Terminal> line = faking::terminal();
    Seam<Clipboard> kept;

    Wired() {
        line->environment["SSH_CONNECTION"] = "10.0.0.1 22 10.0.0.2 22";

        kept = clipboard(Seam<Console>{line});
    }
};

std::string spouted(const std::string& what) {
    Wired one;

    (void)one.kept.provider().copy(what);

    return one.line->written;
}

}

// --- Co pamięta program --------------------------------------------------------------------------

TRIAL("Clipboard: skopiowane wraca przy wklejaniu") {
    Wired one;

    ought(one.kept.provider().copy("ala ma kota"));

    const std::optional<std::string> got = one.kept.provider().paste();

    ought(got.has_value());
    same(*got, std::string{"ala ma kota"});
}

TRIAL("Clipboard: pustka nie jest treścią") {
    Wired one;

    ought(one.kept.provider().copy(""));
    ought(!one.kept.provider().paste().has_value());
}

// Próg dotyczy **wyłącznie drogi przez terminal**: OSC 52 bywa po cichu ucinane, więc lepiej
// powiedzieć „nie poszło" niż wstawić ogryzek udający całość. Cokolwiek powie terminal, **kopia
// zostaje u nas pełna** i wraca w całości.
TRIAL("Clipboard: treść ponad próg zostaje u nas w całości") {
    Wired one;

    const std::string big(200 * 1024, 'x');

    ought(!one.kept.provider().copy(big));

    const std::optional<std::string> got = one.kept.provider().paste();

    ought(got.has_value());
    same(got->size(), big.size());
}

TRIAL("Clipboard: znaki wielobajtowe przechodzą bez zmian") {
    Wired one;

    const std::string what = "z\xc4\x85\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87";

    ought(one.kept.provider().copy(what));

    const std::optional<std::string> got = one.kept.provider().paste();

    ought(got.has_value());
    same(got.value_or(""), what);
}

// Własna kopia należy do schowka, nie do procesu: dwa schowki na dwóch konsolach nie widzą swoich
// kopii. Wspólny jest dopiero ten domyślny.
TRIAL("Clipboard: dwa schowki nie dzielą kopii") {
    Wired one;
    Wired two;

    ought(one.kept.provider().copy("abc"));
    ought(!two.kept.provider().paste().has_value());
}

TRIAL("Clipboard: domyślny jest jeden na proces") {
    ought(&clipboard().provider() == &clipboard().provider());
}

// --- Co idzie do terminala -----------------------------------------------------------------------

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

// Próg liczy się włącznie: dokładnie tyle jeszcze wychodzi.
TRIAL("Clipboard: treść równa progowi jeszcze wychodzi") {
    Wired one;

    ought(one.kept.provider().copy(std::string(64 * 1024, 'x')));
    ought(!one.line->written.empty());
}

#ifndef _WIN32

namespace {

// Program schowka na niby: katalog z `wl-copy` na początku PATH. Udaje albo zapis do pliku, albo
// odmowę; PATH i katalog wracają, jak były.
class Helper {
public:
    explicit Helper(bool works, const std::string& named = "wl-copy", bool stuck = false) {
        where_ = std::filesystem::temp_directory_path() / ("talcite-schowek-" + std::to_string(::getpid()));

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
        std::filesystem::create_directories(where_, ignored);

        {
            std::ofstream making{where_ / named, std::ios::binary};

            making << "#!/bin/sh\n" << (stuck ? "sleep 5\n" : "")
                   << (works ? "cat > \"" + (where_ / "dostał").string() + "\"\n" : "exit 1\n");
        }

        std::filesystem::permissions(where_ / named, std::filesystem::perms::owner_all, ignored);

        const char* was = std::getenv("PATH");

        was_ = was == nullptr ? std::string{} : std::string{was};

        ::setenv("PATH", (where_.string() + ":" + was_).c_str(), 1);
    }

    ~Helper() {
        ::setenv("PATH", was_.c_str(), 1);

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
    }

    Helper(const Helper&) = delete;
    Helper& operator=(const Helper&) = delete;

    [[nodiscard]] std::string got() const {
        std::ifstream reading{where_ / "dostał", std::ios::binary};

        return std::string{std::istreambuf_iterator<char>{reading}, std::istreambuf_iterator<char>{}};
    }

private:
    std::filesystem::path where_;
    std::string was_;
};

}

// W sesji graficznej na miejscu kopia idzie przez program schowka tej sesji — terminale z rodziny
// VTE OSC 52 nie znają. Gdy program odmówi, zostaje OSC 52.
TRIAL("Clipboard: na miejscu kopia idzie przez program schowka sesji") {
    {
        const Helper helper{true};

        auto line = faking::terminal();

        line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        ought(kept.provider().copy("zażółć"));

        same(helper.got(), std::string{"zażółć"});
        same(line->written, std::string{});
    }

    {
        const Helper helper{false};

        auto line = faking::terminal();

        line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        ought(kept.provider().copy("abc"));

        same(line->written, std::string{"\x1b]52;c;YWJj\x07"});
    }

    // Pod X: xclip, a bez niego xsel.
    for (const std::string named : {"xclip", "xsel"}) {
        const Helper helper{true, named};

        auto line = faking::terminal();

        line->environment["DISPLAY"] = ":0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        ought(kept.provider().copy("pod X"));

        same(helper.got(), std::string{"pod X"});
        same(line->written, std::string{});
    }

    // Program, który stanął, ustępuje po półtorej sekundy miejsca OSC 52.
    {
        const Helper helper{true, "wl-copy", true};

        auto line = faking::terminal();

        line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        const Seam<Clipboard> kept = clipboard(Seam<Console>{line});

        const auto began = std::chrono::steady_clock::now();

        ought(kept.provider().copy("abc"));

        ought(std::chrono::steady_clock::now() - began < std::chrono::seconds{4});
        same(line->written, std::string{"\x1b]52;c;YWJj\x07"});
    }

    // Przez ssh program schowka tamtej maszyny nie jest schowkiem użytkownika.
    {
        const Helper helper{true};

        Wired one;

        one.line->environment["WAYLAND_DISPLAY"] = "wayland-0";

        ought(one.kept.provider().copy("abc"));

        same(helper.got(), std::string{});
        same(one.line->written, std::string{"\x1b]52;c;YWJj\x07"});
    }
}

#endif
