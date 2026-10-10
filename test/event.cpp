#include "proving.hpp"

#include <string>

#include "talcite/event.hpp"

using namespace talcite;

TRIAL("Key: an empty key carries nothing") {
    constexpr ui::Key one;

    ought(one.empty());
    same(static_cast<int>(one.letter()), 0);
    same(static_cast<int>(one.code()), static_cast<int>(ui::Code::none));
}

TRIAL("Key: a character compares directly with a character") {
    constexpr ui::Key one{U'q'};

    ought(!one.empty());
    ought(one == U'q');
    ought(!(one == U'w'));
    same(static_cast<int>(one.letter()), static_cast<int>(U'q'));
    same(static_cast<int>(one.code()), static_cast<int>(ui::Code::none));
}

TRIAL("Key: a code compares directly with a code") {
    constexpr ui::Key one{ui::Code::f10};

    ought(!one.empty());
    ought(one == ui::Code::f10);
    ought(!(one == ui::Code::f11));
    same(static_cast<int>(one.code()), static_cast<int>(ui::Code::f10));
    same(static_cast<int>(one.letter()), 0);
}

TRIAL("Key: a character and a code are never confused") {
    constexpr ui::Key letter{U'a'};
    constexpr ui::Key code{ui::Code::up};

    ought(!(letter == code));
    ought(!(letter == ui::Code::up));
    ought(!(code == U'a'));
}

TRIAL("Key: no code falls into the Unicode range") {
    constexpr char32_t last = 0x10FFFF;

    for (int at = 1; at <= static_cast<int>(ui::Code::f12); ++at) {
        const ui::Key one{static_cast<ui::Code>(at)};

        ought(one.letter() == 0);
        ought(!(one == last));
        same(static_cast<int>(one.code()), at);
    }
}

TRIAL("Key: the code none means empty, not a key") {
    constexpr ui::Key one{ui::Code::none};

    ought(one.empty());
    ought(one == ui::Key{});
}

TRIAL("Key: a character outside Latin works too") {
    constexpr ui::Key one{U'ł'};

    ought(one == U'ł');
    ought(!(one == U'l'));
}

TRIAL("Key: a key works at compile time") {
    static_assert(ui::Key{U'x'} == U'x');
    static_assert(ui::Key{ui::Code::enter} == ui::Code::enter);
    static_assert(ui::Key{}.empty());
    static_assert(ui::Key{ui::Code::tab}.letter() == 0);

    ought(true);
}

// Modifiers are part of the key, not something next to it. That is why `Ctrl+C` does not equal `C`,
// and the event's `mods()` simply returns what the key carries: one source of truth instead of two
// that could drift apart.
TRIAL("Event: a key event carries the key together with its modifiers") {
    const ui::KeyEvent what{ui::Key{U'c', ui::Mods{.ctrl = true}}};

    ought(what.key() == (ui::Key{U'c', ui::Mods{.ctrl = true}}));
    ought(!(what.key() == ui::Key{U'c'}));
    same(what.key().letter(), U'c');
    ought(what.mods().ctrl);
    ought(!what.mods().alt);
    ought(!what.mods().shift);
    ought(what.target() == nullptr);
}

TRIAL("Event: modifiers are off by default") {
    const ui::KeyEvent what{ui::Key{ui::Code::escape}};

    ought(!what.mods().ctrl);
    ought(!what.mods().alt);
    ought(!what.mods().shift);
}

TRIAL("Event: a mouse event carries the position, the kind and the button") {
    const ui::MouseEvent what{ui::Spot{12, 4}, ui::Press::down, ui::Mouse::right};

    same(what.where().x, 12);
    same(what.where().y, 4);
    same(static_cast<int>(what.press()), static_cast<int>(ui::Press::down));
    same(static_cast<int>(what.button()), static_cast<int>(ui::Mouse::right));
}

TRIAL("Event: a click and a double click carry the same") {
    const ui::ClickEvent one{ui::Spot{3, 7}, ui::Mouse::left};
    const ui::DoubleClickEvent two{ui::Spot{3, 7}, ui::Mouse::left};

    same(one.where().x, two.where().x);
    same(one.where().y, two.where().y);
    same(static_cast<int>(one.button()), static_cast<int>(two.button()));
}

TRIAL("Event: the wheel carries a signed step") {
    const ui::WheelEvent up{ui::Spot{1, 1}, 3};
    const ui::WheelEvent down{ui::Spot{1, 1}, -3};

    same(up.steps(), 3);
    same(down.steps(), -3);
}

TRIAL("Event: a resize carries the width and the height") {
    const ui::SizeEvent what{80, 25};

    same(what.wide(), 80);
    same(what.high(), 25);
}

TRIAL("Event: a paste carries the text") {
    const ui::PasteEvent what{"treść z schowka"};

    same(what.text(), std::string{"treść z schowka"});
}

TRIAL("Event: focus carries the other side of the change") {
    const ui::FocusEvent taken{nullptr};
    const ui::UnfocusEvent lost{nullptr};

    ought(taken.other() == nullptr);
    ought(lost.other() == nullptr);
}
