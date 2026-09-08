#include "proving.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "talcite/root.hpp"

using namespace talcite;

namespace {

class Leaf : public ui::Node {
public:
    explicit Leaf(std::string named, std::vector<std::string>* told = nullptr,
                  int* alive = nullptr)
        : named_(std::move(named)), told_(told), alive_(alive) {
        if (alive_ != nullptr) ++*alive_;
    }

    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] int takes() const override { return ui::plenty; }

    ~Leaf() override {
        if (alive_ != nullptr) --*alive_;
    }

protected:
    bool react(ui::KeyEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back(named_);

        return false;
    }

    bool react(ui::SizeEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back(named_ + ":rozmiar");

        return false;
    }

private:
    std::string named_;
    std::vector<std::string>* told_ = nullptr;
    int* alive_ = nullptr;
};

class Crown : public ui::Root {
public:
    explicit Crown(std::vector<std::string>* told = nullptr) : told_(told) {}

    [[nodiscard]] int takes() const override { return ui::plenty; }

protected:
    bool react(ui::KeyEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back("korzeń");

        return false;
    }

    bool react(ui::SizeEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back("korzeń:rozmiar");

        return false;
    }

private:
    std::vector<std::string>* told_ = nullptr;
};

std::shared_ptr<Leaf> leaf(const std::string& named, std::vector<std::string>* told = nullptr,
                           int* alive = nullptr) {
    return std::make_shared<Leaf>(named, told, alive);
}

}

TRIAL("Root: świeży korzeń nie ma ogniska") {
    const Crown top;

    ought(top.focus() == nullptr);
}

TRIAL("Root: korzeń jest własnym korzeniem") {
    Crown top;

    ought(top.root() == &top);
}

TRIAL("Root: węzeł w drzewie zna korzeń, poza drzewem nie") {
    Crown top;

    const std::shared_ptr<Leaf> mid = leaf("środek");
    const std::shared_ptr<Leaf> low = leaf("spód");

    ought(mid->root() == nullptr);

    top.children().add(mid);
    mid->children().add(low);

    ought(mid->root() == &top);
    ought(low->root() == &top);

    (void)top.children().remove(mid.get());

    ought(mid->root() == nullptr);
    ought(low->root() == nullptr);
}

TRIAL("Root: drzewo bez korzenia nie ma korzenia") {
    ui::Node up;

    const std::shared_ptr<Leaf> down = leaf("dół");

    up.children().add(down);

    ought(down->root() == nullptr);
}

TRIAL("Root: ognisko daje się ustawić i odczytać z obu stron") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    top.children().add(one);

    ought(!one->focused());

    one->focus();

    ought(top.focus() == one.get());
    ought(one->focused());
}

TRIAL("Root: ognisko da się zgasić") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    top.children().add(one);
    one->focus();

    top.focus(nullptr);

    ought(top.focus() == nullptr);
    ought(!one->focused());
}

TRIAL("Root: węzeł z cudzego drzewa nie dostanie ogniska") {
    Crown first;
    Crown second;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    first.children().add(one);

    threw(second.focus(one.get()));
    ought(second.focus() == nullptr);
}

TRIAL("Root: węzeł poza drzewem nie weźmie ogniska") {
    const std::shared_ptr<Leaf> one = leaf("jeden");

    threw(one->focus());
    ought(!one->focused());
}

TRIAL("Root: klawisz idzie do ogniska i wędruje w górę") {
    std::vector<std::string> told;

    Crown top{&told};

    const std::shared_ptr<Leaf> mid = leaf("środek", &told);
    const std::shared_ptr<Leaf> low = leaf("spód", &told);

    top.children().add(mid);
    mid->children().add(low);

    low->focus();

    ui::KeyEvent what{U'a'};

    ought(!top.deal(what));
    same(told.size(), std::size_t{3});
    same(told[0], std::string{"spód"});
    same(told[1], std::string{"środek"});
    same(told[2], std::string{"korzeń"});
    ought(what.at() == low.get());
}

TRIAL("Root: klawisz bez ogniska idzie do korzenia") {
    std::vector<std::string> told;

    Crown top{&told};

    top.children().add(leaf("jeden", &told));

    ui::KeyEvent what{U'a'};

    ought(!top.deal(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"korzeń"});
    ought(what.at() == &top);
}

TRIAL("Root: rozmiar zawsze idzie do korzenia, nawet przy ognisku") {
    std::vector<std::string> told;

    Crown top{&told};

    const std::shared_ptr<Leaf> one = leaf("jeden", &told);

    top.children().add(one);
    one->focus();

    ui::SizeEvent what{80, 25};

    ought(!top.deal(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"korzeń:rozmiar"});
}

TRIAL("Root: deal oddaje to, co powiedziała wędrówka") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    top.children().add(one);
    one->focus();

    (void)one->on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    ui::KeyEvent what{U'a'};

    ought(top.deal(what));
}

TRIAL("Root: zdjęcie ogniskowanego węzła gasi ognisko") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    top.children().add(one);
    one->focus();

    (void)top.children().remove(one.get());

    ought(top.focus() == nullptr);
}

TRIAL("Root: zdjęcie przodka ogniskowanego węzła też gasi ognisko") {
    Crown top;

    const std::shared_ptr<Leaf> mid = leaf("środek");
    const std::shared_ptr<Leaf> low = leaf("spód");

    top.children().add(mid);
    mid->children().add(low);
    low->focus();

    (void)top.children().remove(mid.get());

    ought(top.focus() == nullptr);
}

TRIAL("Root: zdjęcie węzła obok ogniska niczego nie gasi") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");
    const std::shared_ptr<Leaf> two = leaf("dwa");

    top.children().add(one);
    top.children().add(two);
    one->focus();

    (void)top.children().remove(two.get());

    ought(top.focus() == one.get());
}

TRIAL("Root: zdjęcie poza doręczaniem zwalnia od razu") {
    int alive = 0;

    Crown top;

    top.children().add(leaf("jeden", nullptr, &alive));

    same(alive, 1);

    (void)top.children().remove(top.children().front().get());

    same(alive, 0);
}

TRIAL("Root: węzeł zamykający się w trakcie własnej reakcji nie wywala wędrówki") {
    std::vector<std::string> told;
    int alive = 0;

    Crown top{&told};

    const std::shared_ptr<Leaf> mid = leaf("środek", &told, &alive);

    top.children().add(mid);

    const std::shared_ptr<Leaf> low = leaf("spód", &told, &alive);

    mid->children().add(low);
    low->focus();

    Leaf* stale = mid.get();

    (void)low->on<ui::KeyEvent>([&top, stale](ui::KeyEvent&) {
        (void)top.children().remove(stale);

        return false;
    });

    same(alive, 2);

    ui::KeyEvent what{U'a'};

    ought(!top.deal(what));

    // Uchwyt zdjął gałąź, po której wędrówka właśnie szła. Doszła mimo to do końca po dawnej
    // trasie, a węzły dożyły końca doręczania.
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"spód"});
    same(told[1], std::string{"środek"});
}

TRIAL("Root: odłożone zwolnienie następuje po doręczaniu") {
    int alive = 0;

    Crown top;

    const std::shared_ptr<Leaf> mid = leaf("środek", nullptr, &alive);

    top.children().add(mid);

    Leaf* stale = mid.get();

    (void)top.on<ui::KeyEvent>([&top, stale](ui::KeyEvent&) {
        (void)top.children().remove(stale);

        return false;
    });

    same(alive, 1);

    ui::KeyEvent what{U'a'};

    ought(!top.deal(what));

    same(alive, 1);
}

TRIAL("Root: odłożone zwolnienie naprawdę zwalnia") {
    int alive = 0;

    Crown top;

    {
        const std::shared_ptr<Leaf> mid = leaf("środek", nullptr, &alive);

        top.children().add(mid);
    }

    Leaf* stale = static_cast<Leaf*>(top.children().front().get());

    (void)top.on<ui::KeyEvent>([&top, stale](ui::KeyEvent&) {
        (void)top.children().remove(stale);

        return false;
    });

    same(alive, 1);

    ui::KeyEvent what{U'a'};

    ought(!top.deal(what));

    same(alive, 0);
}

TRIAL("Root: wyjątek z uchwytu nie zostawia korzenia w stanie doręczania") {
    int alive = 0;

    Crown top;

    {
        const std::shared_ptr<Leaf> mid = leaf("środek", nullptr, &alive);

        top.children().add(mid);
    }

    Leaf* stale = static_cast<Leaf*>(top.children().front().get());

    const ui::Handle which =
        top.on<ui::KeyEvent>([](ui::KeyEvent&) -> bool {
            throw std::runtime_error("uchwyt się wywrócił");
        });

    ui::KeyEvent what{U'a'};

    threw((void)top.deal(what));

    top.off<ui::KeyEvent>(which);

    // Gdyby licznik doręczania został podniesiony przez wyjątek, zdjęcie odłożyłoby się
    // zamiast zwolnić od razu — i to jest jedyny sposób, żeby to zobaczyć z zewnątrz.
    same(alive, 1);

    (void)top.children().remove(stale);

    same(alive, 0);
}

namespace {

// Węzeł zapisujący, co go spotkało przy ognisku, i z kim po drugiej stronie.
class Watched : public ui::Node {
public:
    explicit Watched(std::string named, std::vector<std::string>* told)
        : named_(std::move(named)), told_(told) {}

    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] int takes() const override { return ui::plenty; }

    [[nodiscard]] const ui::Node* other() const { return other_; }

    [[nodiscard]] bool held() const { return held_; }

protected:
    bool react(ui::FocusEvent& what) override {
        told_->push_back(named_ + ":objął");
        other_ = what.other();
        held_ = focused();

        return false;
    }

    bool react(ui::UnfocusEvent& what) override {
        told_->push_back(named_ + ":stracił");
        other_ = what.other();
        held_ = focused();

        return false;
    }

private:
    std::string named_;
    std::vector<std::string>* told_ = nullptr;
    const ui::Node* other_ = nullptr;
    bool held_ = false;
};

}

TRIAL("Root: objęcie ogniska wysyła zdarzenie") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);

    top.children().add(one);
    one->focus();

    same(told.size(), std::size_t{1});
    same(told[0], std::string{"jeden:objął"});
    ought(one->other() == nullptr);
}

TRIAL("Root: przeniesienie ogniska wysyła najpierw utratę, potem objęcie") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);
    const auto two = std::make_shared<Watched>("dwa", &told);

    top.children().add(one);
    top.children().add(two);

    one->focus();
    told.clear();

    two->focus();

    same(told.size(), std::size_t{2});
    same(told[0], std::string{"jeden:stracił"});
    same(told[1], std::string{"dwa:objął"});
}

TRIAL("Root: zdarzenia ogniska niosą drugą stronę zmiany") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);
    const auto two = std::make_shared<Watched>("dwa", &told);

    top.children().add(one);
    top.children().add(two);

    one->focus();
    two->focus();

    ought(one->other() == two.get());
    ought(two->other() == one.get());
}

TRIAL("Root: w obu zdarzeniach ognisko jest już przestawione") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);
    const auto two = std::make_shared<Watched>("dwa", &told);

    top.children().add(one);
    top.children().add(two);

    one->focus();
    two->focus();

    // Tracący pyta focused() i ma już usłyszeć „nie", obejmujący — „tak".
    ought(!one->held());
    ought(two->held());
}

TRIAL("Root: zgaszenie ogniska wysyła tylko utratę") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);

    top.children().add(one);
    one->focus();
    told.clear();

    top.focus(nullptr);

    same(told.size(), std::size_t{1});
    same(told[0], std::string{"jeden:stracił"});
    ought(one->other() == nullptr);
}

TRIAL("Root: ustawienie tego samego ogniska nic nie wysyła") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);

    top.children().add(one);
    one->focus();
    told.clear();

    one->focus();
    top.focus(one.get());

    same(told.size(), std::size_t{0});
}

TRIAL("Root: zdarzenie ogniska wędruje w górę jak każde inne") {
    std::vector<std::string> told;

    Crown top;

    const auto mid = std::make_shared<Watched>("środek", &told);
    const auto low = std::make_shared<Watched>("spód", &told);

    top.children().add(mid);
    mid->children().add(low);

    bool seen = false;

    (void)mid->on<ui::FocusEvent>([&seen](ui::FocusEvent&) {
        seen = true;

        return false;
    });

    low->focus();

    ought(seen);
}
