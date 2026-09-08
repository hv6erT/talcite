#include "proving.hpp"

#include <string>

#include "talcite/event.hpp"

using namespace talcite;

TRIAL("Key: pusty klawisz niczego nie niesie") {
    constexpr ui::Key one;

    ought(!one.held());
    same(static_cast<int>(one.letter()), 0);
    same(static_cast<int>(one.code()), static_cast<int>(ui::Code::none));
}

TRIAL("Key: znak porównuje się wprost ze znakiem") {
    constexpr ui::Key one{U'q'};

    ought(one.held());
    ought(one == U'q');
    ought(!(one == U'w'));
    same(static_cast<int>(one.letter()), static_cast<int>(U'q'));
    same(static_cast<int>(one.code()), static_cast<int>(ui::Code::none));
}

TRIAL("Key: kod porównuje się wprost z kodem") {
    constexpr ui::Key one{ui::Code::f10};

    ought(one.held());
    ought(one == ui::Code::f10);
    ought(!(one == ui::Code::f11));
    same(static_cast<int>(one.code()), static_cast<int>(ui::Code::f10));
    same(static_cast<int>(one.letter()), 0);
}

TRIAL("Key: znak i kod nigdy się nie mylą") {
    constexpr ui::Key letter{U'a'};
    constexpr ui::Key code{ui::Code::up};

    ought(!(letter == code));
    ought(!(letter == ui::Code::up));
    ought(!(code == U'a'));
}

TRIAL("Key: żaden kod nie wpada w zakres Unicode") {
    constexpr char32_t last = 0x10FFFF;

    for (int at = 1; at <= static_cast<int>(ui::Code::f12); ++at) {
        const ui::Key one{static_cast<ui::Code>(at)};

        ought(one.letter() == 0);
        ought(!(one == last));
        same(static_cast<int>(one.code()), at);
    }
}

TRIAL("Key: kod none znaczy pustkę, a nie klawisz") {
    constexpr ui::Key one{ui::Code::none};

    ought(!one.held());
    ought(one == ui::Key{});
}

TRIAL("Key: znak spoza łaciny też przechodzi") {
    constexpr ui::Key one{U'ł'};

    ought(one == U'ł');
    ought(!(one == U'l'));
}

TRIAL("Key: klawisz działa w czasie kompilacji") {
    static_assert(ui::Key{U'x'} == U'x');
    static_assert(ui::Key{ui::Code::enter} == ui::Code::enter);
    static_assert(!ui::Key{}.held());
    static_assert(ui::Key{ui::Code::tab}.letter() == 0);

    ought(true);
}

// **Modyfikatory są częścią klawisza**, a nie czymś obok niego. Dlatego `Ctrl+C` nie równa się
// `C`, a `mods()` zdarzenia oddaje po prostu to, co niesie klawisz — jedno źródło prawdy zamiast
// dwóch, które dawało się rozejść.
TRIAL("Event: zdarzenie klawisza niesie klawisz razem z modyfikatorami") {
    const ui::KeyEvent what{ui::Key{U'c', ui::Mods{.ctrl = true}}};

    ought(what.key() == (ui::Key{U'c', ui::Mods{.ctrl = true}}));
    ought(!(what.key() == ui::Key{U'c'}));
    same(what.key().letter(), U'c');
    ought(what.mods().ctrl);
    ought(!what.mods().alt);
    ought(!what.mods().shift);
    ought(what.at() == nullptr);
}

TRIAL("Event: modyfikatory domyślnie milczą") {
    const ui::KeyEvent what{ui::Key{ui::Code::escape}};

    ought(!what.mods().ctrl);
    ought(!what.mods().alt);
    ought(!what.mods().shift);
}

TRIAL("Event: mysz niesie miejsce, rodzaj i przycisk") {
    const ui::MouseEvent what{ui::Spot{12, 4}, ui::Press::down, ui::Mouse::right};

    same(what.where().x, 12);
    same(what.where().y, 4);
    same(static_cast<int>(what.press()), static_cast<int>(ui::Press::down));
    same(static_cast<int>(what.button()), static_cast<int>(ui::Mouse::right));
}

TRIAL("Event: kliknięcie i podwójne kliknięcie niosą to samo") {
    const ui::ClickEvent one{ui::Spot{3, 7}, ui::Mouse::left};
    const ui::DoubleClickEvent two{ui::Spot{3, 7}, ui::Mouse::left};

    same(one.where().x, two.where().x);
    same(one.where().y, two.where().y);
    same(static_cast<int>(one.button()), static_cast<int>(two.button()));
}

TRIAL("Event: kółko niesie skok ze znakiem") {
    const ui::WheelEvent up{ui::Spot{1, 1}, 3};
    const ui::WheelEvent down{ui::Spot{1, 1}, -3};

    same(up.steps(), 3);
    same(down.steps(), -3);
}

TRIAL("Event: rozmiar niesie szerokość i wysokość") {
    const ui::SizeEvent what{80, 25};

    same(what.wide(), 80);
    same(what.high(), 25);
}

TRIAL("Event: wklejenie niesie treść") {
    const ui::PasteEvent what{"treść z schowka"};

    same(what.said(), std::string{"treść z schowka"});
}

TRIAL("Event: ognisko niesie drugą stronę zmiany") {
    const ui::FocusEvent taken{nullptr};
    const ui::UnfocusEvent lost{nullptr};

    ought(taken.other() == nullptr);
    ought(lost.other() == nullptr);
}
