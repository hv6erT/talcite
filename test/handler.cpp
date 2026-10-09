#include "proving.hpp"

#include <string>
#include <vector>

#include "talcite/handler.hpp"

using namespace talcite;

namespace {

// Najprostszy możliwy węzeł: umie mieć rodzica i umie wpuścić zdarzenie. Wszystko, czego
// Handler potrzebuje do wędrówki, i nic ponadto.
class Knot : public ui::Handler {
public:
    explicit Knot(std::vector<std::string>& told, std::string named)
        : told_(told), named_(std::move(named)) {}

    void under(Knot* one) { up_ = one; }

    [[nodiscard]] ui::Handler* parent() const override { return up_; }

    void takes(bool given) { takes_ = given; }

    template <typename One>
    bool send(One& what) {
        return trigger(what);
    }

    using ui::Handler::trigger;

protected:
    bool handle(ui::KeyEvent& what) override {
        (void)what;

        told_.push_back(named_ + ":handle");

        return takes_;
    }

    bool handle(ui::MouseEvent& what) override {
        (void)what;

        told_.push_back(named_ + ":mysz");

        return takes_;
    }

private:
    std::vector<std::string>& told_;
    std::string named_;
    Knot* up_ = nullptr;
    bool takes_ = false;
};

}

TRIAL("Handler: handle celu dostaje zdarzenie") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};
    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"jeden:handle"});
}

TRIAL("Handler: nieobsłużone idzie do rodzica") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    ui::KeyEvent what{U'a'};

    ought(!down.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"dół:handle"});
    same(told[1], std::string{"góra:handle"});
}

TRIAL("Handler: obsłużone nie idzie wyżej") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);
    down.takes(true);

    ui::KeyEvent what{U'a'};

    ought(down.send(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"dół:handle"});
}

TRIAL("Handler: at wskazuje na pierwszy węzeł, nie na ostatni") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);
    up.takes(true);

    ui::KeyEvent what{U'a'};

    ought(down.send(what));
    ought(what.target() == &down);
}

TRIAL("Handler: uchwyt z on dostaje zdarzenie, gdy handle go nie wziął") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"jeden:handle"});
    same(told[1], std::string{"uchwyt"});
}

// **Dwa kanały, dwa niezależne zatrzymania.** Widżet i wołający wędrują tą samą drogą, ale każdy
// zamyka wyłącznie swój ruch: prawda z `handle` mówi „dość reagowania w górę", prawda z uchwytu mówi
// „dość uchwytów w górę", i żadne z tych zdań nie mówi nic o drugim kanale. Wcześniej niosła to
// jedna wartość i znaczyła dwie rzeczy naraz — stąd brało się to, że widżet zasłaniał wołającemu
// zdarzenie, którego wołający miał pełne prawo zobaczyć.
TRIAL("Handler: handle, który wziął, nie ucisza uchwytów") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    one.takes(true);

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(one.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"jeden:handle"});
    same(told[1], std::string{"uchwyt"});
}

TRIAL("Handler: wszystkie uchwyty biegną, nawet gdy pierwszy wziął") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("pierwszy");

        return true;
    });

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(one.send(what));
    same(told.size(), std::size_t{3});
    same(told[1], std::string{"pierwszy"});
    same(told[2], std::string{"drugi"});
}

// Prawda z uchwytu zamyka **drogę uchwytów**, a nie całą wędrówkę: `handle` przodka i tak się
// odezwie. Dzięki temu uchwyt wołającego nigdy nie oślepia widżetu, który stoi nad nim — a to jest
// dokładnie ten przypadek, przez który przycisk komunikatu potrafił połknąć własne naciśnięcie.
TRIAL("Handler: uchwyt, który wziął, zatrzymuje tylko swój kanał") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    int seen = 0;

    (void)down.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    (void)up.on<ui::KeyEvent>([&seen](ui::KeyEvent&) {
        ++seen;

        return true;
    });

    ui::KeyEvent what{U'a'};

    ought(down.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"dół:handle"});
    same(told[1], std::string{"góra:handle"});

    // Uchwyt na górze już nie biegnie — jego kanał zamknął się na dole.
    same(seen, 0);
}

TRIAL("Handler: uchwyty innego rodzaju milczą") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    (void)one.on<ui::MouseEvent>([&told](ui::MouseEvent&) {
        told.push_back("mysz");

        return true;
    });

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"jeden:handle"});
}

TRIAL("Handler: off zdejmuje uchwyt") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook which = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return true;
    });

    one.off(which);

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{1});
}

// **Uchwyt zna swój typ i wie, czy coś przez niego wisi.** Pomyłki w typie przy odpinaniu już nie
// ma jak zrobić; pusty jest świeży i odpięty, a dopiero `on` daje niepusty.
TRIAL("Handler: uchwyt jest pusty świeży i po odpięciu, a pełny po dopięciu") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    const ui::Hook fresh;

    ought(fresh.empty());

    ui::Hook which = one.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    ought(!which.empty());

    ui::Hook copy = which;

    one.off(which);

    ought(which.empty());

    // Kopia wskazuje to samo, co już odpięte: jej odpięcie nic nie robi i niczego nie psuje.
    ought(!copy.empty());

    one.off(copy);

    ought(copy.empty());

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
}

// Uchwyt tego samego numeru przy innym typie nie zdejmuje cudzego — typ jest w uchwycie, nie w
// wołaniu, więc odpina się dokładnie to, co dopięto.
TRIAL("Handler: uchwyt odpina tylko swój typ") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook mouse = one.on<ui::MouseEvent>([](ui::MouseEvent&) { return true; });

    (void)one.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    one.off(mouse);

    ui::KeyEvent what{U'a'};

    ought(one.send(what));
}

TRIAL("Handler: off zdejmuje jeden, a nie wszystkie") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook first = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("pierwszy");

        return false;
    });

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    one.off(first);

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{2});
    same(told[1], std::string{"drugi"});
}

TRIAL("Handler: off na pustym uchwycie niczego nie psuje") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook none;

    one.off(none);
    one.off(none);

    ought(none.empty());

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
}

TRIAL("Handler: uchwyt widzi cel przez at") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    const ui::Handler* seen = nullptr;

    (void)up.on<ui::KeyEvent>([&seen](ui::KeyEvent& what) {
        seen = what.target();

        return true;
    });

    ui::KeyEvent what{U'a'};

    ought(down.send(what));
    ought(seen == &down);
}

TRIAL("Handler: wędrówka przez trzy szczeble") {
    std::vector<std::string> told;

    Knot top{told, "szczyt"};
    Knot mid{told, "środek"};
    Knot low{told, "spód"};

    low.under(&mid);
    mid.under(&top);

    top.takes(true);

    ui::KeyEvent what{U'a'};

    ought(low.send(what));
    same(told.size(), std::size_t{3});
    same(told[0], std::string{"spód:handle"});
    same(told[1], std::string{"środek:handle"});
    same(told[2], std::string{"szczyt:handle"});
}

TRIAL("Handler: rodzaje wędrują tą samą drogą") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    ui::MouseEvent what{ui::Spot{}, ui::Press::down, ui::Mouse::left};

    ought(!down.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"dół:mysz"});
    same(told[1], std::string{"góra:mysz"});
}

TRIAL("Handler: uchwyt odpięty w trakcie własnego biegu nie wywala pętli") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook which;

    which = one.on<ui::KeyEvent>([&one, &which, &told](ui::KeyEvent&) {
        told.push_back("pierwszy");
        one.off(which);

        return false;
    });

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{3});
    same(told[2], std::string{"drugi"});
}

TRIAL("Handler: węzeł odpięty od rodzica w trakcie handle i tak oddaje wyżej") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    (void)down.on<ui::KeyEvent>([&down](ui::KeyEvent&) {
        down.under(nullptr);

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(!down.send(what));
    same(told.size(), std::size_t{2});
    same(told[1], std::string{"góra:handle"});
}

// **Zdarzenie programu idzie wyłącznie kanałem wołających.** Kanał widżetu zna tylko zdarzenia
// biblioteki — dla typu spoza zestawu `carried` go nawet nie otwiera — a uchwyty dostają je
// jak każde inne: w górę, do pierwszego, który weźmie.
namespace {

class Own : public ui::Event {};

class Mute : public ui::Handler {
public:
    void under(ui::Handler* one) { up_ = one; }

    [[nodiscard]] ui::Handler* parent() const override { return up_; }

    template <typename One>
    bool send(One& what) {
        return trigger(what);
    }

private:
    ui::Handler* up_ = nullptr;
};

}

TRIAL("Handler: zdarzenie programu wędruje w górę do uchwytów") {
    Mute under;
    Mute middle;
    Mute above;

    under.under(&middle);
    middle.under(&above);

    std::vector<std::string> heard;

    (void)middle.on<Own>([&heard](Own&) {
        heard.emplace_back("środek");

        return false;
    });
    (void)above.on<Own>([&heard](Own&) {
        heard.emplace_back("góra");

        return true;
    });

    Own what;

    ought(under.send(what));
    same(heard.size(), std::size_t{2});
    same(heard[1], std::string{"góra"});
}

TRIAL("Handler: zdarzenie programu, którego nikt nie wziął, oddaje fałsz") {
    Mute under;
    Mute above;

    under.under(&above);

    int heard = 0;

    (void)above.on<Own>([&heard](Own&) {
        ++heard;

        return false;
    });

    Own what;

    ought(!under.send(what));
    same(heard, 1);
}
