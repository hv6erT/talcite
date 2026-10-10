#include <chrono>
#include <iostream>
#include <string>

#include "talcite/capabilities.hpp"

#include "speaking.hpp"

namespace {

// How many colors the terminal shows, in words.
std::string named(talcite::Colors given) {
    switch (given) {
        case talcite::Colors::mono: return "none";
        case talcite::Colors::ansi: return "16 colors";
        case talcite::Colors::palette: return "256 colors";
        case talcite::Colors::rgb: return "true color";
    }

    return "?";
}

// Which mouse reporting the terminal offers, in words.
std::string named(talcite::Mouse given) {
    switch (given) {
        case talcite::Mouse::none: return "none";
        case talcite::Mouse::basic: return "basic";
        case talcite::Mouse::sgr: return "SGR";
    }

    return "?";
}

// Which alternate screen the terminal has, with its private mode number.
std::string named(talcite::Screen given) {
    switch (given) {
        case talcite::Screen::none: return "none";
        case talcite::Screen::older: return "old (47)";
        case talcite::Screen::apart: return "alternate (1049)";
    }

    return "?";
}

// Whether the terminal's background is light or dark, in words.
std::string named(talcite::Shade given) {
    switch (given) {
        case talcite::Shade::neutral: return "unknown";
        case talcite::Shade::light: return "light";
        case talcite::Shade::dark: return "dark";
    }

    return "?";
}

// Which pixel graphics protocol the terminal speaks, in words.
std::string named(talcite::Graphics given) {
    switch (given) {
        case talcite::Graphics::none: return "none";
        case talcite::Graphics::sixel: return "sixel";
        case talcite::Graphics::kitty: return "kitty protocol";
    }

    return "?";
}

// The densest block characters the terminal can show, with their grid.
std::string named(talcite::ui::Blocks given) {
    switch (given) {
        case talcite::ui::Blocks::basic: return "block characters";
        case talcite::ui::Blocks::sextants: return "sextants (2x3)";
        case talcite::ui::Blocks::octants: return "octants (2x4)";
    }

    return "?";
}

// A yes or a no.
std::string named(bool given) { return given ? "yes" : "no"; }

// The number of characters in UTF-8 text: we count characters, not bytes. Aligning by the size of
// the string would misalign the columns on non-ASCII letters, which is exactly the problem this
// whole class measures widths for.
std::size_t letters(const std::string& said) {
    std::size_t made = 0;

    for (const char sign : said)
        if ((static_cast<unsigned char>(sign) & 0xC0) != 0x80) ++made;

    return made;
}

// Prints one line of the report: the label padded to 22 characters, then the value.
void shown(const std::string& what, const std::string& told) {
    const std::size_t much = letters(what);

    std::cout << "  " << what << std::string(much < 22 ? 22 - much : 1, ' ') << told << "\n";
}

// The text with ESC written as \e and other control bytes as \x escapes, so that it can be printed
// safely.
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

// Probes the terminal and prints everything `Capabilities` learned, with the time the probe took.
int main() {
    const showing::Speaking speaking;

    const auto began = std::chrono::steady_clock::now();

    talcite::Capabilities compat;

    // The first question starts the probe; the rest are already cached.
    const talcite::Colors colors = compat.colors();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began);

    std::cout << "\nWhat the terminal says\n\n";

    shown("name", compat.named().empty() ? "(not given)" : compat.named());
    shown("version", std::to_string(compat.version()));
    shown("color", named(colors));
    shown("mouse", named(compat.mouse()));
    shown("alternate screen", named(compat.screen()));
    shown("background", named(compat.shade()));
    shown("bracketed paste", named(compat.bracketed_paste()));
    shown("focus reporting", named(compat.focus_events()));
    shown("synchronized output", named(compat.synchronized()));
    shown("Unicode", named(compat.unicode()));
    shown("window title", named(compat.titles()));

    std::cout << "\nImages\n\n";

    shown("pixel protocol", named(compat.graphics()));

    const talcite::ui::Size cell = compat.cell();

    shown("cell in pixels", cell.wide == 0 || cell.high == 0
                                    ? std::string{"(not given)"}
                                    : std::to_string(cell.wide) + " x " +
                                          std::to_string(cell.high));

    // The measurement only tells whether a character takes one cell, not whether the font has it.
    // So this is a ceiling: the densest grid the terminal will not scatter. The application makes
    // the choice through `Setup`, and `Term` takes the smaller of the two.
    shown("block drawing", named(compat.blocks()));

    std::cout << "\nWidths in columns\n\n";

    shown("Chinese character", std::to_string(compat.wide()));
    shown("emoji with selector", std::to_string(compat.varied()));
    shown("joined emoji", std::to_string(compat.joined()));

    std::cout << "\nThe probe took " << spent.count() << " ms\n";

    if (!compat.rest().empty())
        std::cout << "Bytes not for us: " << plainly(compat.rest()) << "\n";

    std::cout << "\n";

    return 0;
}
