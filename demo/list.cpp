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
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"

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

std::string named(int which) {
    const std::string number = std::to_string(which);

    if (which % 7 == 0) return "katalog-" + number;

    if (which % 5 == 0) return "bardzo-dluga-nazwa-pliku-ktora-sie-nie-zmiesci-" + number + ".txt";

    return "plik-" + number + ".txt";
}

// Okno z listą: wybór jednej pozycji. **Tab chodzi pole → lista → przyciski**, a strzałki w środku
// listy Tabu nie zabierają — lista jest dla wędrówki jednym przystankiem, choć w środku ma sto
// chętnych wierszy.
std::shared_ptr<talcite::ui::Dialog> asking(const std::shared_ptr<talcite::ui::Span>& told) {
    const auto made = std::make_shared<talcite::ui::Dialog>();

    const auto frame = std::make_shared<talcite::ui::Frame>(
        "Wybór pliku", talcite::ui::FrameShape{talcite::ui::Border::twin});

    const auto column = std::make_shared<talcite::ui::Column>();

    column->children().add(std::make_shared<talcite::ui::Span>("Nazwa:"));

    const auto field = std::make_shared<talcite::ui::Field>(34);

    column->children().add(field);
    column->children().add(std::make_shared<talcite::ui::Span>(" "));

    const auto list = std::make_shared<talcite::ui::List>(34, 8, talcite::ui::ScrollShape{true});

    for (int which = 0; which < 40; ++which)
        list->children().add(std::make_shared<talcite::ui::ListItem>(named(which)));

    // Wybrany wiersz wpisuje się do pola. To jest **`ScrollEvent`, a nie `PickEvent`**: odzywa się
    // przy każdej zmianie wiersza, a nie dopiero przy zatwierdzeniu. Wiersza zdarzenie nie niesie —
    // słuchacz bierze go z `current()`, bo lista i tak go trzyma.
    const std::weak_ptr<talcite::ui::List> weakly = list;

    (void)list->on<talcite::ui::ScrollEvent>([field, weakly](talcite::ui::ScrollEvent&) {
        if (const auto one = weakly.lock())
            if (const talcite::ui::ListItem* item = one->current()) field->text(item->text());

        return false;
    });

    column->children().add(list);
    column->children().add(std::make_shared<talcite::ui::Span>(" "));

    const auto yes = std::make_shared<talcite::ui::Button>("Otwórz");
    const auto no = std::make_shared<talcite::ui::Button>("Przerwij");

    yes->keys().add(U'O');
    no->keys().add(U'P');
    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);
    column->children().add(row);

    frame->children().add(column);
    made->children().add(frame);

    const std::weak_ptr<talcite::ui::Dialog> weak = made;

    const auto shutting = [weak, told, field](const std::string& said) {
        return [weak, told, field, said] {
            told->text(said + field->text());

            if (const auto one = weak.lock()) one->close();
        };
    };

    (void)yes->on<talcite::ui::PickEvent>([what = shutting("otwarte: ")](talcite::ui::PickEvent&) {
        what();

        return true;
    });
    (void)no->on<talcite::ui::PickEvent>(
        [what = shutting("przerwane, w polu zostało: ")](talcite::ui::PickEvent&) {
            what();

            return true;
        });

    // Enter i dwuklik na wierszu robią to samo co przycisk podstawowy.
    for (const std::shared_ptr<talcite::ui::Node>& one : list->children())
        if (const auto item = std::dynamic_pointer_cast<talcite::ui::ListItem>(one))
            (void)item->on<talcite::ui::PickEvent>(
                [what = shutting("otwarte: ")](talcite::ui::PickEvent&) {
                    what();

                    return true;
                });

    return made;
}

}

// Pełny ekran: lista wypełnia ramkę, pasek z prawej pokazuje, gdzie jesteśmy, a wiersz nad nią
// mówi, co jest bieżące. F3 otwiera okno, w którym ta sama lista stoi obok pola i przycisków.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto dialogs = std::make_shared<talcite::ui::Modals>();
    const auto legend = std::make_shared<talcite::ui::Legend>();

    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — lista");
    const auto column = std::make_shared<talcite::ui::Column>();
    const auto told = std::make_shared<talcite::ui::Span>("strzałki, PgUp, PgDn, Home, End, kółko");

    const auto list = std::make_shared<talcite::ui::List>(talcite::ui::plenty, talcite::ui::plenty,
                                                       talcite::ui::ScrollShape{true});

    for (int which = 0; which < 200; ++which)
        list->children().add(std::make_shared<talcite::ui::ListItem>(named(which)));

    // Co dziesiąta pozycja niedostępna — wędrówka ma je omijać, a nie zatrzymywać się na nich.
    for (int which = 0; which < 200; which += 10)
        if (const auto one = std::dynamic_pointer_cast<talcite::ui::ListItem>(
                list->children()[static_cast<std::size_t>(which)]))
            one->enabled(false);

    const std::weak_ptr<talcite::ui::List> weakly = list;

    (void)list->on<talcite::ui::ScrollEvent>([told, weakly](talcite::ui::ScrollEvent&) {
        if (const auto one = weakly.lock())
            if (const talcite::ui::ListItem* item = one->current())
                told->text("wiersz " + std::to_string(one->which().value_or(0)) + ": " + item->text());

        return false;
    });

    for (const std::shared_ptr<talcite::ui::Node>& one : list->children())
        if (const auto item = std::dynamic_pointer_cast<talcite::ui::ListItem>(one)) {
            const std::weak_ptr<talcite::ui::ListItem> weak = item;

            (void)item->on<talcite::ui::PickEvent>([told, weak](talcite::ui::PickEvent&) {
                if (const auto which = weak.lock()) told->text("otwarte: " + which->text());

                return true;
            });
        }

    column->children().add(told);
    column->children().add(list);
    frame->children().add(column);

    legend->content(frame);
    dialogs->content(legend);

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Wybór"),
                      [dialogs, told] { dialogs->open(demo::closing(asking(told))); });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(dialogs);

    list->focus();

    term.show(sheet);
    term.title("talcite — lista");

    term.run();

    return 0;
}
