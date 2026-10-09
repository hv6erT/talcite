#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/line.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/pane.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/scroll.hpp"

#include "doing.hpp"

namespace {

class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

private:
    talcite::Term& term_;
};

// Pudełko dialogu to **zwykła rama z treścią**. Dialog robi mu trzy rzeczy: środkuje, rzuca cień
// i połyka wszystko, co w nie nie trafiło. Podwójna kreska jak w Midnight Commanderze.
std::shared_ptr<talcite::ui::Dialog> asking(const std::string& title, const std::string& said,
                                         const std::shared_ptr<talcite::ui::Span>& told) {
    const auto made = std::make_shared<talcite::ui::Dialog>();

    const auto frame =
        std::make_shared<talcite::ui::Frame>(title, talcite::ui::FrameShape{talcite::ui::Border::twin});

    const auto column = std::make_shared<talcite::ui::Column>();

    column->children().add(std::make_shared<talcite::ui::Span>(said));
    column->children().add(std::make_shared<talcite::ui::Span>(" "));

    // Tab chodzi między przyciskami, Enter naciska ten pod ogniskiem, klawisz aktywny działa
    // z dowolnego miejsca okna, Esc zamyka. Oznaczenie podstawowego rysuje klamrę ze strzałkami
    // i nic poza tym: naciśnięcie należy do ogniska.
    const auto yes = std::make_shared<talcite::ui::Button>("Tak");
    const auto no = std::make_shared<talcite::ui::Button>("Nie");
    const auto stop = std::make_shared<talcite::ui::Button>("Przerwij");

    // Napis jest czysty, klawisze aktywne podaje się osobno. Litera, która w napisie występuje,
    // podkreśla się sama.
    yes->keys().add(U'T');
    no->keys().add(U'N');
    stop->keys().add(std::vector<talcite::ui::Key>{U'P', talcite::ui::Code::f10});

    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);
    row->children().add(stop);

    column->children().add(row);

    frame->children().add(column);
    made->children().add(frame);

    const std::weak_ptr<talcite::ui::Dialog> weak = made;

    const auto answered = [weak, told](std::string what) {
        return [weak, told, what = std::move(what)] {
            told->text("odpowiedź: " + what);

            if (const auto one = weak.lock()) one->close();
        };
    };

    (void)yes->on<talcite::ui::PickEvent>([what = answered("Tak")](talcite::ui::PickEvent&) {
        what();

        return true;
    });
    (void)no->on<talcite::ui::PickEvent>([what = answered("Nie")](talcite::ui::PickEvent&) {
        what();

        return true;
    });
    (void)stop->on<talcite::ui::PickEvent>([what = answered("Przerwij")](talcite::ui::PickEvent&) {
        what();

        return true;
    });

    return made;
}

// Okno z polem: pytanie o nazwę. **Enter zatwierdza, Esc zamyka, Tab uzupełnia podpowiedź** —
// a gdy nie ma czego uzupełniać, przechodzi do przycisku.
std::shared_ptr<talcite::ui::Dialog> named(const std::shared_ptr<talcite::ui::Span>& told) {
    const auto made = std::make_shared<talcite::ui::Dialog>();

    const auto frame = std::make_shared<talcite::ui::Frame>(
        "Przemianowanie", talcite::ui::FrameShape{talcite::ui::Border::twin});

    const auto column = std::make_shared<talcite::ui::Column>();

    column->children().add(std::make_shared<talcite::ui::Span>("Nowa nazwa (spr\u00f3buj: ka, do, pl):"));

    const auto field = std::make_shared<talcite::ui::Field>(30);

    // Podpowiedź liczy wołający. Tu z garści nazw; w prawdziwym programie z katalogu — ale wtedy
    // **z gotowej listy, nie z wyprawy na dysk**, bo to jest wołane przy każdej zmianie treści.
    field->guess([](const std::string& what) {
        static const char* these[] = {"katalog", "kabel", "dokumenty", "pliki.txt"};

        if (what.empty()) return std::string{};

        for (const char* one : these) {
            const std::string which{one};

            if (which.size() > what.size() && which.compare(0, what.size(), what) == 0)
                return which;
        }

        return std::string{};
    });

    column->children().add(field);

    // Rozdzielnik dochodzi do ramki: złączki `├` i `┤` domalowuje sama ramka, bo dziecko nie ma
    // prawa malować poza swoim prostokątem.
    column->children().add(std::make_shared<talcite::ui::Line>("Co dalej"));

    const auto yes = std::make_shared<talcite::ui::Button>("Zmień");
    const auto no = std::make_shared<talcite::ui::Button>("Przerwij");

    yes->keys().add(U'Z');
    no->keys().add(U'P');
    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);
    column->children().add(row);

    frame->children().add(column);
    made->children().add(frame);

    const std::weak_ptr<talcite::ui::Dialog> weak = made;

    (void)yes->on<talcite::ui::PickEvent>([weak, told, field](talcite::ui::PickEvent&) {
        told->text("nowa nazwa: " + field->text());

        if (const auto one = weak.lock()) one->close();

        return true;
    });

    (void)no->on<talcite::ui::PickEvent>([weak, told](talcite::ui::PickEvent&) {
        told->text("przemianowanie przerwane");

        if (const auto one = weak.lock()) one->close();

        return true;
    });

    return made;
}

}

// Pełny ekran w kolejności, która wynika z tego, co ma leżeć na czym: dialog zakrywa legendę,
// legenda zakrywa pasek menu, pasek menu zakrywa treść.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto dialogs = std::make_shared<talcite::ui::Modals>();
    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto menus = std::make_shared<talcite::ui::Menubar>();

    // **Klawisz otwierający podaje aplikacja** — biblioteka żadnego sobie nie przypisuje.
    menus->keys().add(talcite::ui::Key{talcite::ui::Code::f9});


    const auto pane = std::make_shared<talcite::ui::Pane>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — okno dialogowe");
    const auto scroll = std::make_shared<talcite::ui::Scroll>(talcite::ui::Axis::vertical, talcite::ui::ScrollShape{true});
    const auto column = std::make_shared<talcite::ui::Column>();
    const auto said =
        std::make_shared<talcite::ui::Span>("F5 pyta tak/nie, F6 pyta o nazwę z podpowiedzią; Tab, "
                                         "Enter, litera i Esc działają w oknie");

    column->children().add(said);

    for (int which = 0; which < 60; ++which)
        column->children().add(std::make_shared<talcite::ui::Span>("wiersz numer " + std::to_string(which)));

    scroll->children().add(column);
    frame->children().add(scroll);
    pane->children().add(frame);
    menus->content(pane);
    legend->content(menus);
    dialogs->content(legend);

    const auto file = menus->add("Plik");

    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Kopiuj", "F5"), [dialogs, said] {
        dialogs->open(demo::closing(asking("Kopiowanie", "Skopiować zaznaczone pliki?", said)));
    }));
    file->children().add(std::make_shared<talcite::ui::MenuSeparator>());
    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Wyjście", "F10"),
                          [&term] { term.stop(); }));

    (void)menus->add("Widok");

    (void)demo::doing(legend->add(talcite::ui::Code::f1, "Pomoc"), [dialogs, said] {
        dialogs->open(
            asking("Pomoc", "Dialog jest modalny: pod spodem nic nie odpowiada.", said));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Kopiuj"), [dialogs, said] {
        dialogs->open(demo::closing(asking("Kopiowanie", "Skopiować zaznaczone pliki?", said)));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f6, "PrzNaz"),
                      [dialogs, said] { dialogs->open(demo::closing(named(said))); });

    (void)demo::doing(legend->add(talcite::ui::Code::f9, "MenuGr"), [menus] {
        if (!menus->menus().empty()) menus->menus().front()->focus();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(dialogs);

    pane->focus();

    term.show(sheet);
    term.title("talcite — okno dialogowe");

    term.run();

    return 0;
}
