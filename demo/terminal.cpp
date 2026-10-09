#include <chrono>
#include <iostream>
#include <string>

#include "talcite/capabilities.hpp"

#include "speaking.hpp"

namespace {

std::string named(talcite::Colors given) {
    switch (given) {
        case talcite::Colors::mono: return "brak";
        case talcite::Colors::ansi: return "16 kolorów";
        case talcite::Colors::palette: return "256 kolorów";
        case talcite::Colors::rgb: return "pełna paleta";
    }

    return "?";
}

std::string named(talcite::Mouse given) {
    switch (given) {
        case talcite::Mouse::none: return "brak";
        case talcite::Mouse::basic: return "zwykła";
        case talcite::Mouse::sgr: return "SGR";
    }

    return "?";
}

std::string named(talcite::Screen given) {
    switch (given) {
        case talcite::Screen::none: return "brak";
        case talcite::Screen::older: return "stary (47)";
        case talcite::Screen::apart: return "zapasowy (1049)";
    }

    return "?";
}

std::string named(talcite::Shade given) {
    switch (given) {
        case talcite::Shade::neutral: return "nie wiadomo";
        case talcite::Shade::light: return "jasne";
        case talcite::Shade::dark: return "ciemne";
    }

    return "?";
}

std::string named(talcite::Graphics given) {
    switch (given) {
        case talcite::Graphics::none: return "brak";
        case talcite::Graphics::sixel: return "sixel";
        case talcite::Graphics::kitty: return "protokół kitty";
    }

    return "?";
}

std::string named(talcite::ui::Blocks given) {
    switch (given) {
        case talcite::ui::Blocks::basic: return "znaki blokowe";
        case talcite::ui::Blocks::sextants: return "sekstanty (2x3)";
        case talcite::ui::Blocks::octants: return "oktanty (2x4)";
    }

    return "?";
}

std::string named(bool given) { return given ? "tak" : "nie"; }

// Liczymy znaki, nie bajty. Pierwsza wersja wyrównywała po rozmiarze napisu i kolumny
// rozjeżdżały się na polskich literach — czyli dokładnie ten kłopot, dla którego cała ta
// klasa mierzy szerokości.
std::size_t letters(const std::string& said) {
    std::size_t made = 0;

    for (const char sign : said)
        if ((static_cast<unsigned char>(sign) & 0xC0) != 0x80) ++made;

    return made;
}

void shown(const std::string& what, const std::string& told) {
    const std::size_t much = letters(what);

    std::cout << "  " << what << std::string(much < 22 ? 22 - much : 1, ' ') << told << "\n";
}

std::string plainly(const std::string& given) {
    std::string made;

    for (const char sign : given) {
        if (sign == '\x1b') {
            made += "\\e";
            continue;
        }

        if (sign >= 0x20 && sign != 0x7F) {
            made += sign;
            continue;
        }

        made += "\\x";
        made += "0123456789abcdef"[(static_cast<unsigned char>(sign) >> 4) & 0x0F];
        made += "0123456789abcdef"[static_cast<unsigned char>(sign) & 0x0F];
    }

    return made;
}

}

int main() {
    const showing::Speaking speaking;

    const auto began = std::chrono::steady_clock::now();

    talcite::Capabilities compat;

    // Pierwsze pytanie odpala badanie; reszta jest już buforowana.
    const talcite::Colors colors = compat.colors();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began);

    std::cout << "\nCo mówi terminal\n\n";

    shown("nazwa", compat.named().empty() ? "(nie podał)" : compat.named());
    shown("wersja", std::to_string(compat.version()));
    shown("kolor", named(colors));
    shown("mysz", named(compat.mouse()));
    shown("ekran zapasowy", named(compat.screen()));
    shown("tło", named(compat.shade()));
    shown("wklejanie w nawiasach", named(compat.bracketed_paste()));
    shown("zgłaszanie ogniska", named(compat.focus_events()));
    shown("rysowanie uzgodnione", named(compat.synchronized()));
    shown("Unicode", named(compat.unicode()));
    shown("tytuł okna", named(compat.titles()));

    std::cout << "\nObrazy\n\n";

    shown("protokół pikselowy", named(compat.graphics()));

    const talcite::ui::Size cell = compat.cell();

    shown("kratka w pikselach", cell.wide == 0 || cell.high == 0
                                    ? std::string{"(nie podał)"}
                                    : std::to_string(cell.wide) + " x " +
                                          std::to_string(cell.high));

    // **Pomiar mówi tylko, czy znak zajmuje jedną kratkę** — nie czy font go ma. Dlatego to jest
    // sufit: gęstość, której terminal jeszcze nie rozsypie. Wyboru dokonuje aplikacja przez
    // `Setup`, a `Term` bierze mniejszą z dwóch.
    shown("znaki rysunkowe", named(compat.blocks()));

    std::cout << "\nSzerokości w kolumnach\n\n";

    shown("znak chiński", std::to_string(compat.wide()));
    shown("emoji ze znacznikiem", std::to_string(compat.varied()));
    shown("emoji złożone", std::to_string(compat.joined()));

    std::cout << "\nBadanie trwało " << spent.count() << " ms\n";

    if (!compat.rest().empty())
        std::cout << "Bajty nie od nas: " << plainly(compat.rest()) << "\n";

    std::cout << "\n";

    return 0;
}
