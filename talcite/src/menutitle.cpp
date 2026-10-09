#include "menutitle.hpp"

#include "talcite/menu.hpp"
#include "talcite/menubar.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "marking.hpp"

namespace talcite::ui {

namespace {

// Podświetlony tytuł to tytuł z ogniskiem — pasek czynny, zwinięty — albo tytuł menu rozwiniętego.
bool lit(const MenuTitle& one) { return one.focused() || one.menu().opened(); }

}

MenuTitle::MenuTitle(Menu& menu) : menu_(menu) {}

// Napis z kratką odstępu po obu stronach — tyle, ile zajmował w pasku, gdy pasek liczył to sam.
Size MenuTitle::wants(Size room) const {
    return {std::min(2 + measured(menu_.title()).wide, room.wide), std::min(1, room.high)};
}

// Tytuł bierze ognisko, bo pasek czynny musi mieć je gdzieś postawić, a zwinięte menu jest poza
// drzewem i wziąć go nie może.
bool MenuTitle::focusable() const { return true; }

View MenuTitle::paint(Size given, Area seen) {
    View made{seen.size};

    const Skin& how = skin();

    // Role bierzemy z menu, nie własne: dla skórki tytuł i lista to jedno menu, a `talcite.menubar`
    // było rolą tytułu od zawsze.
    const Style look = how.style(lit(*this) ? menu_.role() + ".focus" : menu_.role());

    made.fill({{-seen.at.x, -seen.at.y}, {given.wide, 1}}, look);
    made.put({1 - seen.at.x, -seen.at.y}, menu_.title(), look);

    // Litera aktywna — ta sama maszyneria, co w pozycji menu i w przycisku. Tytuł stoi o jedną
    // kratkę od lewej krawędzi swojego pasa, więc i litera jest o tę kratkę przesunięta.
    talcite::ui::marking(made, {1 - seen.at.x, -seen.at.y}, menu_.title(),
                         talcite::ui::shortcut_column(menu_.keys(), menu_.title()),
                         look, how.style(menu_.role() + ".key"));

    return made;
}

// **Zwinięte z ogniskiem rozwija się w dół i Enterem**, jak w Midnight Commanderze; reszta idzie
// w górę do paska — strzałki w bok, Esc i litery tytułów. Tytuł bez pozycji pod spodem: Enter woła
// czynność tytułu, bo nie ma czego rozwijać.
bool MenuTitle::handle(KeyEvent& what) {
    if (!focused()) return false;

    const Mods mods = what.mods();

    if (mods.ctrl || mods.alt) return false;

    const Code code = what.key().code();

    if (code == Code::enter) {
        if (menu_.children().empty()) menu_.pick();
        else menu_.open();

        return true;
    }

    if (code == Code::down && !menu_.children().empty()) {
        menu_.open();

        return true;
    }

    return false;
}

// **Czynność tytułu to rozwinięcie menu.** Litera robi dokładnie to, co kliknięcie w tytuł —
// i nic ponadto. Klawisze są w menu, nie tu, żeby `menu->keys().add(...)` działało jak dotąd.
bool MenuTitle::handle(ShortcutEvent& what) {
    if (!menu_.keys().contains(what.key())) return false;

    menu_.open();

    return true;
}

// Klik w tytuł przełącza, jak w Midnight Commanderze. Klik **rozwija**, a nie tylko uaktywnia:
// myszą człowiek celuje w konkretne menu i chce jego zawartości. Klawiaturą jest inaczej — F9
// bierze pasek, a rozwinięcie wybiera się osobno.
bool MenuTitle::handle(ClickEvent&) {
    if (menu_.children().empty()) {
        menu_.pick();

        return true;
    }

    if (menu_.opened()) {
        if (menu_.bar_ != nullptr) menu_.bar_->leave();

        return true;
    }

    if (root() != nullptr) menu_.open();

    return true;
}

}
