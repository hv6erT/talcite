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
    bool react(ui::KeyEvent& what) override {
        (void)what;

        told_.push_back(named_ + ":react");

        return takes_;
    }

    bool react(ui::MouseEvent& what) override {
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

TRIAL("Handler: react celu dostaje zdarzenie") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};
    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"jeden:react"});
}

TRIAL("Handler: nieobsłużone idzie do rodzica") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    ui::KeyEvent what{U'a'};

    ought(!down.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"dół:react"});
    same(told[1], std::string{"góra:react"});
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
    same(told[0], std::string{"dół:react"});
}

TRIAL("Handler: at wskazuje na pierwszy węzeł, nie na ostatni") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);
    up.takes(true);

    ui::KeyEvent what{U'a'};

    ought(down.send(what));
    ought(what.at() == &down);
}

TRIAL("Handler: uchwyt z on dostaje zdarzenie, gdy react go nie wziął") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"jeden:react"});
    same(told[1], std::string{"uchwyt"});
}

// **Dwa kanały, dwa niezależne zatrzymania.** Widżet i wołający wędrują tą samą drogą, ale każdy
// zamyka wyłącznie swój ruch: prawda z `react` mówi „dość reagowania w górę", prawda z uchwytu mówi
// „dość uchwytów w górę", i żadne z tych zdań nie mówi nic o drugim kanale. Wcześniej niosła to
// jedna wartość i znaczyła dwie rzeczy naraz — stąd brało się to, że widżet zasłaniał wołającemu
// zdarzenie, którego wołający miał pełne prawo zobaczyć.
TRIAL("Handler: react, który wziął, nie ucisza uchwytów") {
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
    same(told[0], std::string{"jeden:react"});
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

// Prawda z uchwytu zamyka **drogę uchwytów**, a nie całą wędrówkę: `react` przodka i tak się
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
    same(told[0], std::string{"dół:react"});
    same(told[1], std::string{"góra:react"});

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
    same(told[0], std::string{"jeden:react"});
}

TRIAL("Handler: off zdejmuje uchwyt") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    const ui::Handle which = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return true;
    });

    one.off<ui::KeyEvent>(which);

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{1});
}

TRIAL("Handler: off pod niewłaściwym rodzajem nic nie zdejmuje") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    const ui::Handle which = one.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    one.off<ui::MouseEvent>(which);

    ui::KeyEvent what{U'a'};

    ought(one.send(what));
}

TRIAL("Handler: off zdejmuje jeden, a nie wszystkie") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    const ui::Handle first = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("pierwszy");

        return false;
    });

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    one.off<ui::KeyEvent>(first);

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{2});
    same(told[1], std::string{"drugi"});
}

TRIAL("Handler: off na pustym i zerowym numerze niczego nie psuje") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    one.off<ui::KeyEvent>(0);
    one.off<ui::KeyEvent>(7);

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
        seen = what.at();

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
    same(told[0], std::string{"spód:react"});
    same(told[1], std::string{"środek:react"});
    same(told[2], std::string{"szczyt:react"});
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

    ui::Handle which = 0;

    which = one.on<ui::KeyEvent>([&one, &which, &told](ui::KeyEvent&) {
        told.push_back("pierwszy");
        one.off<ui::KeyEvent>(which);

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

TRIAL("Handler: węzeł odpięty od rodzica w trakcie react i tak oddaje wyżej") {
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
    same(told[1], std::string{"góra:react"});
}
