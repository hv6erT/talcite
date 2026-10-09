#include "proving.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "talcite/node.hpp"

using namespace talcite;

namespace {

// Węzeł z policzalnym życiem i z dostępem do tego, co w Node chronione. Tyle wystarczy, żeby
// sprawdzić drzewo i to, jak przez nie wędrują zdarzenia.
class Twig : public ui::Node {
public:
    explicit Twig(std::string named, std::vector<std::string>* told = nullptr,
                  int* alive = nullptr)
        : named_(std::move(named)), told_(told), alive_(alive) {
        if (alive_ != nullptr) ++*alive_;
    }

    ~Twig() override {
        if (alive_ != nullptr) --*alive_;
    }

    using ui::Node::broadcast;

    template <typename One>
    bool send(One& what) {
        return trigger(what);
    }

    [[nodiscard]] std::size_t much() const { return children().many(); }

    [[nodiscard]] const ui::Node* first() const {
        return children().empty() ? nullptr : children().front().get();
    }

    void eats(bool given) { eats_ = given; }

    [[nodiscard]] int capacity() const override { return ui::plenty; }

protected:
    bool handle(ui::KeyEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back(named_);

        return eats_;
    }

private:
    std::string named_;
    std::vector<std::string>* told_ = nullptr;
    int* alive_ = nullptr;
    bool eats_ = false;
};

std::shared_ptr<Twig> twig(const std::string& named, std::vector<std::string>* told = nullptr,
                           int* alive = nullptr) {
    return std::make_shared<Twig>(named, told, alive);
}

}

TRIAL("Node: dopięte dziecko zna rodzica") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> down = twig("dół");

    up.children().add(down);

    ought(down->parent() == &up);
    same(up.much(), std::size_t{1});
    ought(up.first() == down.get());
}

// `index` oddaje numer dziecka wśród dzieci — i pustkę dla węzła, który dzieckiem nie jest,
// także po odpięciu. Lista, menu i pasek menu liczyły to każde własną pętlą.
TRIAL("Node: index zna numer dziecka i nie zna obcego") {
    Twig top{"góra"};

    const std::shared_ptr<Twig> first = twig("pierwsze");
    const std::shared_ptr<Twig> second = twig("drugie");
    const std::shared_ptr<Twig> stranger = twig("obce");

    top.children().add(first);
    top.children().add(second);

    same(top.children().find(*first), std::optional<std::size_t>{0});
    same(top.children().find(*second), std::optional<std::size_t>{1});
    ought(!top.children().find(*stranger).has_value());
    ought(!top.children().find(top).has_value());

    (void)top.children().remove(first.get());

    ought(!top.children().find(*first).has_value());
    same(top.children().find(*second), std::optional<std::size_t>{0});
}

TRIAL("Node: świeży węzeł nie ma rodzica ani dzieci") {
    const Twig one{"sam"};

    ought(one.parent() == nullptr);
    same(one.much(), std::size_t{0});
}

TRIAL("Node: dopięcie pustki odrzuca") {
    Twig up{"góra"};

    threw(up.children().add(nullptr));
}

TRIAL("Node: węzeł z rodzicem nie da się dopiąć drugi raz") {
    Twig first{"pierwszy"};
    Twig second{"drugi"};

    const std::shared_ptr<Twig> down = twig("dół");

    first.children().add(down);

    threw(second.children().add(down));
    ought(down->parent() == &first);
    same(second.much(), std::size_t{0});
}

TRIAL("Node: węzeł nie może być własnym dzieckiem") {
    const std::shared_ptr<Twig> one = twig("sam");

    threw(one->children().add(one));
}

TRIAL("Node: węzeł nie może dostać własnego przodka") {
    const std::shared_ptr<Twig> top = twig("szczyt");
    const std::shared_ptr<Twig> mid = twig("środek");
    const std::shared_ptr<Twig> low = twig("spód");

    top->children().add(mid);
    mid->children().add(low);

    threw(low->children().add(top));
    threw(low->children().add(mid));
}

TRIAL("Node: remove oddaje dziecko i zrywa wskazanie na rodzica") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> down = twig("dół");

    up.children().add(down);

    const std::shared_ptr<ui::Node> taken = up.children().remove(down.get());

    ought(taken == down);
    ought(down->parent() == nullptr);
    same(up.much(), std::size_t{0});
}

TRIAL("Node: remove cudzego węzła oddaje pustkę") {
    Twig up{"góra"};
    Twig apart{"obok"};

    const std::shared_ptr<Twig> down = twig("dół");

    apart.children().add(down);

    ought(up.children().remove(down.get()) == nullptr);
    ought(up.children().remove(nullptr) == nullptr);
    ought(down->parent() == &apart);
}

// --- Dzieci hurtem --------------------------------------------------------------------------------

// Składanie okna to zwykle kilkoro dzieci pod rząd, więc wchodzą jednym wywołaniem — a kolejność
// zostaje ta, w jakiej je podano.
TRIAL("Node: hurtowe dopięcie wpuszcza wszystkie i zachowuje kolejność") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");
    const std::shared_ptr<Twig> three = twig("trzy");

    up.children().add(std::vector<std::shared_ptr<ui::Node>>{one, two, three});

    same(up.much(), std::size_t{3});
    ought(up.children()[0].get() == one.get());
    ought(up.children()[1].get() == two.get());
    ought(up.children()[2].get() == three.get());
}

// **Albo wszystkie, albo żadne.** Zwykły węzeł przyjmuje jedno dziecko, więc drugie z pary odpada —
// i wtedy pierwsze też ma wyjść, zamiast zostawić wołającemu drzewo złożone do połowy.
TRIAL("Node: hurtowe dopięcie przy błędzie nie zostawia połowy") {
    ui::Node up;

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");

    threw(up.children().add(std::vector<std::shared_ptr<ui::Node>>{one, two}));

    same(up.children().many(), std::size_t{0});
    ought(one->parent() == nullptr);
    ought(two->parent() == nullptr);
}

TRIAL("Node: clear zdejmuje wszystkie dzieci naraz") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");

    up.children().add(one);
    up.children().add(two);

    up.children().clear();

    ought(up.children().empty());
    ought(one->parent() == nullptr);
    ought(two->parent() == nullptr);
}

// Zdjęte dziecko ma zginąć tak samo, jak po pojedynczym `remove` — czyli gdy nikt go już nie trzyma.
TRIAL("Node: clear puszcza dzieci, a nie trzyma ich dalej") {
    int alive = 0;

    Twig up{"góra"};

    up.children().add(twig("raz", nullptr, &alive));
    up.children().add(twig("dwa", nullptr, &alive));

    same(alive, 2);

    up.children().clear();

    same(alive, 0);
}

TRIAL("Node: contains rozpoznaje własne dziecko, a cudzego nie") {
    Twig up{"góra"};
    Twig apart{"obok"};

    const std::shared_ptr<Twig> mine = twig("moje");
    const std::shared_ptr<Twig> theirs = twig("cudze");

    up.children().add(mine);
    apart.children().add(theirs);

    ought(up.children().contains(mine.get()));
    ought(!up.children().contains(theirs.get()));
    ought(!up.children().contains(nullptr));
}

// --- Rozdawanie w głąb -----------------------------------------------------------------------------

TRIAL("Node: rozdanie schodzi po kolei i kończy się na pierwszym, który weźmie") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");
    const std::shared_ptr<Twig> three = twig("trzy");

    up.children().add(one);
    up.children().add(two);
    up.children().add(three);

    std::vector<std::string> said;

    for (const auto& [named, which] :
         {std::pair{std::string{"raz"}, one}, std::pair{std::string{"dwa"}, two},
          std::pair{std::string{"trzy"}, three}})
        (void)which->on<ui::ShortcutEvent>([&said, named](ui::ShortcutEvent&) {
            said.push_back(named);

            return named == "dwa";
        });

    ui::KeyEvent what{U'x'};

    ought(up.broadcast(what));

    same(static_cast<int>(said.size()), 2);
    same(said[0], std::string{"raz"});
    same(said[1], std::string{"dwa"});
}

// **Uchwyt ma prawo przebudować drzewo w trakcie rozdawania.** Chodzenie po żywym wektorze kończyło
// się tu naruszeniem pamięci: dopięcie przenosi bufor, a przejście szło dalej po starym adresie.
// Chodzimy po migawce, więc przenosiny nie mają na co wpłynąć.
TRIAL("Node: rozdanie przeżywa przebudowę drzewa w uchwycie") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");

    up.children().add(one);
    up.children().add(two);

    (void)one->on<ui::ShortcutEvent>([&up](ui::ShortcutEvent&) {
        for (int at = 0; at < 64; ++at) up.children().add(twig("dokładka"));

        return false;
    });

    int done = 0;

    (void)two->on<ui::ShortcutEvent>([&done](ui::ShortcutEvent&) {
        ++done;

        return true;
    });

    ui::KeyEvent what{U'x'};

    ought(up.broadcast(what));

    same(done, 1);
    same(up.much(), std::size_t{66});
}

// To samo od drugiej strony: uchwyt, który **zdejmuje** rodzeństwo, też nie może wywrócić przejścia.
TRIAL("Node: rozdanie przeżywa opróżnienie drzewa w uchwycie") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");

    up.children().add(one);
    up.children().add(two);

    (void)one->on<ui::ShortcutEvent>([&up](ui::ShortcutEvent&) {
        up.children().clear();

        return false;
    });

    int done = 0;

    (void)two->on<ui::ShortcutEvent>([&done](ui::ShortcutEvent&) {
        ++done;

        return true;
    });

    ui::KeyEvent what{U'x'};

    // Zdjęte rodzeństwo dostaje zdarzenie mimo wszystko, bo migawka trzyma je przy życiu — tak samo
    // zachowują się uchwyty w `Handler`.
    ought(up.broadcast(what));

    same(done, 1);
    ought(up.children().empty());
}

TRIAL("Node: przeniesienie pod innego rodzica jednym wyrażeniem") {
    Twig first{"pierwszy"};
    Twig second{"drugi"};

    const std::shared_ptr<Twig> down = twig("dół");

    first.children().add(down);
    second.children().add(first.children().remove(down.get()));

    ought(down->parent() == &second);
    same(first.much(), std::size_t{0});
    same(second.much(), std::size_t{1});
}

TRIAL("Node: zdjęte dziecko ginie, gdy nikt go nie trzyma") {
    int alive = 0;

    {
        Twig up{"góra"};

        up.children().add(twig("dół", nullptr, &alive));

        same(alive, 1);

        (void)up.children().remove(const_cast<ui::Node*>(up.first()));

        same(alive, 0);
    }
}

TRIAL("Node: rodzic zabiera dzieci ze sobą") {
    int alive = 0;

    {
        Twig up{"góra"};

        up.children().add(twig("jeden", nullptr, &alive));
        up.children().add(twig("dwa", nullptr, &alive));

        same(alive, 2);
    }

    same(alive, 0);
}

TRIAL("Node: dziecko przeżywające rodzica nie zostaje z wiszącym wskazaniem") {
    std::shared_ptr<Twig> down = twig("dół");

    {
        Twig up{"góra"};

        up.children().add(down);

        ought(down->parent() == &up);
    }

    ought(down->parent() == nullptr);
}

TRIAL("Node: całe poddrzewo ginie razem z korzeniem gałęzi") {
    int alive = 0;

    {
        Twig up{"góra"};

        const std::shared_ptr<Twig> mid = twig("środek", nullptr, &alive);

        mid->children().add(twig("spód", nullptr, &alive));

        up.children().add(mid);

        same(alive, 2);
    }

    same(alive, 0);
}

TRIAL("Node: zdarzenie wędruje drzewem w górę") {
    std::vector<std::string> told;

    const std::shared_ptr<Twig> top = twig("szczyt", &told);
    const std::shared_ptr<Twig> mid = twig("środek", &told);
    const std::shared_ptr<Twig> low = twig("spód", &told);

    top->children().add(mid);
    mid->children().add(low);

    ui::KeyEvent what{U'a'};

    ought(!low->send(what));
    same(told.size(), std::size_t{3});
    same(told[0], std::string{"spód"});
    same(told[2], std::string{"szczyt"});
    ought(what.target() == low.get());
}

TRIAL("Node: odpięta gałąź nie oddaje zdarzeń dawnemu rodzicowi") {
    std::vector<std::string> told;

    const std::shared_ptr<Twig> top = twig("szczyt", &told);
    const std::shared_ptr<Twig> mid = twig("środek", &told);

    top->children().add(mid);

    (void)top->children().remove(mid.get());

    ui::KeyEvent what{U'a'};

    ought(!mid->send(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"środek"});
}

TRIAL("Node: uchwyt dopięty do rodzica łapie zdarzenie z dziecka") {
    std::vector<std::string> told;

    const std::shared_ptr<Twig> up = twig("góra", &told);
    const std::shared_ptr<Twig> down = twig("dół", &told);

    up->children().add(down);

    const ui::Handler* seen = nullptr;

    (void)up->on<ui::KeyEvent>([&seen](ui::KeyEvent& what) {
        seen = what.target();

        return true;
    });

    ui::KeyEvent what{U'a'};

    ought(down->send(what));
    ought(seen == down.get());
}

TRIAL("Node: świeży węzeł nie ma prostokąta") {
    const Twig one{"sam"};

    same(one.area().at.x, 0);
    same(one.area().at.y, 0);
    same(one.area().size.wide, 0);
    same(one.area().size.high, 0);
}

// Sięgnięcie poza koniec jest błędem wołającego, a nie stanem do przemilczenia — ta sama reguła
// co w zbiorze klawiszy i w zbiorze opcji.
TRIAL("Children: sięgnięcie poza koniec rzuca") {
    const std::shared_ptr<Twig> up = twig("góra");

    threw((void)up->children().front());
    threw((void)up->children().back());
    threw((void)up->children()[0]);

    up->children().add(twig("dół"));

    same(up->children()[0].get(), up->children().front().get());
    same(up->children().front().get(), up->children().back().get());

    threw((void)up->children()[1]);
}
