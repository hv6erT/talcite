#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "talcite/button.hpp"
#include "talcite/bytes.hpp"
#include "talcite/column.hpp"
#include "talcite/comb.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/editor.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/fs.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/line.hpp"
#include "talcite/promise.hpp"
#include "talcite/row.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/tick.hpp"
#include "talcite/view.hpp"

#include "doing.hpp"

namespace {

class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

    std::function<void()> sized;

protected:
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

    bool handle(talcite::ui::SizeEvent&) override {
        if (sized) sized();

        return false;
    }

private:
    talcite::Term& term_;
};

// **Tekst wbudowany.** Edytor ma co pokazać także wtedy, gdy nikt nie podał ścieżki — a przy
// okazji stoi tu wszystko, co potrafi się zepsuć: tabulatory, pusty wiersz, ogonki i wiersz
// dłuższy niż okno.
constexpr const char* sample =
    "talcite — edytor tekstu\n"
    "\n"
    "Dokument leży w tablicy kawałków, więc pliku nikt nie wczytuje w całości. Wpisanie znaku\n"
    "rozcina jeden kawałek na trzy, skasowanie rozcina i wyrzuca, a oryginał zostaje tam, gdzie\n"
    "był — za źródłem. Pamięć rośnie z liczbą zmian, nie z rozmiarem pliku.\n"
    "\n"
    "Klawisze\n"
    "\tF2\tzapis\n"
    "\tF3\tszukanie\n"
    "\tF4\tzamiana\n"
    "\tF5\tzawijanie wierszy\n"
    "\tF7\tcofnięcie\n"
    "\tF8\tponowienie\n"
    "\tF10\twyjście\n"
    "\n"
    "Strzałki chodzą po znakach i wierszach, Home i End po wierszu, PageUp i PageDown po ekranie,\n"
    "a Ctrl ze strzałką po wyrazach. Z klawiszem Shift każdy z nich ciągnie zaznaczenie.\n"
    "\n"
    "Myszą: kliknięcie stawia kursor — także za tabulatorem i w zawiniętej połowie wiersza, bo\n"
    "kolumna nie jest bajtem, a rząd nie jest wierszem. Dwuklik bierze wyraz, a ciągnięcie zaznacza;\n"
    "przy dolnej krawędzi widok schodzi za kursorem tak szybko, jak szybko ruszasz myszą.\n"
    "\n"
    "Schowek\n"
    "\n"
    "\tCtrl+C\tCtrl+Ins\tkopiowanie\n"
    "\tCtrl+X\tShift+Del\twycięcie\n"
    "\tCtrl+V\tShift+Ins\twklejenie\n"
    "\n"
    "Schowek jest **systemowy**, a nie nasz: na Windows idzie do schowka systemu, a na Linuksie\n"
    "i przez ssh prosimy o to terminal, żeby wstawił go do schowka tej maszyny, przy której siedzisz.\n"
    "W drugą stronę wkleja sam terminal swoim skrótem — i to działa niezależnie od powyższych.\n"
    "\n"
    "Cofanie\n"
    "\n"
    "Pisany wyraz cofa się w całości, nie po literze: spacja, tabulator i enter zamykają grupę,\n"
    "a ruch kursora ją gasi. Zamiana wszystkich wystąpień wraca jednym ruchem, choćby zmian było\n"
    "sto. Gwiazdka na górnym pasku znaczy, że dokument ma niezapisane zmiany — i gaśnie sama,\n"
    "gdy cofniesz wszystko z powrotem.\n"
    "\n"
    "Szukanie\n"
    "\n"
    "Grzebień szuka w jednym wierszu, a po dokumencie chodzi edytor — bo tylko on wie, gdzie\n"
    "wiersz się zaczyna. Wzorzec nie przekracza więc końca wiersza. W okienku szukania można\n"
    "włączyć wyrażenie regularne, wielkość liter i całe wyrazy.\n"
    "\n"
    "Zapis\n"
    "\n"
    "Biblioteka nie dotyka plików: źródło daje aplikacja i ujście daje aplikacja. To demo pisze\n"
    "obok, do pliku z końcówką .new, i mówi o tym na pasku — bo pisanie po tym samym pliku,\n"
    "z którego czyta źródło, zniszczyłoby dokument.\n";

talcite::Bytes lump(const std::string& what) {
    talcite::Bytes made;

    made.reserve(what.size());

    for (const char sign : what) made.push_back(static_cast<std::byte>(sign));

    return made;
}

talcite::Style turned() {
    talcite::Style made{};

    made.reverse = true;

    return made;
}

// Okienko szukania: wzorzec i trzy przełączniki. Zły wzorzec **nie jest wyjątkiem** — grzebień
// mówi, że nie jest gotowy, i to trafia na pasek.
//
// Ognisko trafia w pole samo: okno stawia je na pierwszym chętnym, a pole stoi tu przed
// przyciskami.
std::shared_ptr<talcite::ui::Dialog>
hunting(const std::string& title, bool swapping,
        std::function<void(std::string, std::string, talcite::Sift)> after) {
    const auto made = std::make_shared<talcite::ui::Dialog>();

    const auto frame =
        std::make_shared<talcite::ui::Frame>(title, talcite::ui::FrameShape{talcite::ui::Border::twin});

    const auto column = std::make_shared<talcite::ui::Column>();

    column->children().add(std::make_shared<talcite::ui::Span>("Szukany tekst:"));

    const auto what = std::make_shared<talcite::ui::Field>(40);

    column->children().add(what);

    const auto with = std::make_shared<talcite::ui::Field>(40);

    if (swapping) {
        column->children().add(std::make_shared<talcite::ui::Span>("Zamień na:"));
        column->children().add(with);
    }

    column->children().add(std::make_shared<talcite::ui::Line>("Jak"));

    // Trzy niezależne przełączniki. Każdy trzyma swój stan **sam**, więc nie ma czego pilnować
    // z zewnątrz — odczytujemy je dopiero przy zatwierdzeniu okna. Litera aktywna działa
    // z dowolnego miejsca okna i jest w napisie podkreślona.
    const auto regex = std::make_shared<talcite::ui::Tick>("wyrażenie regularne");
    const auto cased = std::make_shared<talcite::ui::Tick>("wielkość liter");
    const auto whole = std::make_shared<talcite::ui::Tick>("całe wyrazy");

    regex->keys().add(U'g');
    cased->keys().add(U'l');
    whole->keys().add(U'c');

    column->children().add(regex);
    column->children().add(cased);
    column->children().add(whole);

    column->children().add(std::make_shared<talcite::ui::Line>("Co dalej"));

    const auto yes = std::make_shared<talcite::ui::Button>(swapping ? "Zamień" : "Szukaj");
    const auto no = std::make_shared<talcite::ui::Button>("Przerwij");

    yes->keys().add(swapping ? U'Z' : U'S');
    no->keys().add(U'P');
    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);
    column->children().add(row);

    frame->children().add(column);
    made->children().add(frame);

    const std::weak_ptr<talcite::ui::Dialog> weak = made;

    (void)yes->on<talcite::ui::PickEvent>(
        [weak, what, with, regex, cased, whole, after = std::move(after)](talcite::ui::PickEvent&) {
            if (after)
                after(what->text(), with->text(),
                     talcite::Sift{regex->ticked(), cased->ticked(), whole->ticked()});

            if (const auto one = weak.lock()) one->close();

            return true;
        });

    (void)no->on<talcite::ui::PickEvent>([weak](talcite::ui::PickEvent&) {
        if (const auto one = weak.lock()) one->close();

        return true;
    });

    return made;
}

}

int main(int argc, char** argv) {
    talcite::Term term;

    Sheet sheet{term};

    sheet.skin(talcite::ui::Skin{{{"demo.bar", turned()}}});

    const auto dialogs = std::make_shared<talcite::ui::Modals>();
    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto bar = std::make_shared<talcite::ui::Span>(std::string{});
    const auto shown = std::make_shared<talcite::ui::Editor>(talcite::ui::ScrollShape{true});

    // **Ctrl+C, Ctrl+X i Ctrl+V dokłada aplikacja**, bo edytor ich sobie nie przypisuje: w trybie
    // surowym Ctrl+C nie jest przerwaniem, tylko zwykłym klawiszem, i widżet połykający go
    // odbierałby aplikacji możliwość użycia go na wyjście. Komplet z Midnight Commandera —
    // Ctrl+Ins, Shift+Ins, Shift+Del — edytor obsługuje sam.
    {
        const std::weak_ptr<talcite::ui::Editor> weak = shown;

        (void)shown->on<talcite::ui::KeyEvent>([weak](talcite::ui::KeyEvent& what) {
            if (!what.mods().ctrl || what.mods().alt) return false;

            const auto mine = weak.lock();

            if (!mine) return false;

            switch (what.key().letter()) {
                case U'c': return mine->copy();
                case U'x': return mine->cut();
                case U'v': return mine->paste();
                default: return false;
            }
        });
    }

    bar->role("demo.bar");
    bar->wrapped(false);

    column->children().add(bar);
    column->children().add(shown);

    legend->content(column);
    dialogs->content(legend);

    sheet.children().add(dialogs);

    const auto source = std::make_shared<std::string>("tekst wbudowany");
    const auto told = std::make_shared<std::string>();

    // Po lewej źródło z gwiazdką, po prawej wiersz i kolumna. Kolumna liczy się **w kratkach**,
    // nie w bajtach, więc za tabulatorem skacze o osiem — dokładnie tak, jak to widać.
    const auto naming = [&term, bar, shown, source, told] {
        std::string left = *source;

        if (shown->dirty()) left += " *";

        if (!told->empty()) left += "   " + *told;

        const std::string right = "wiersz " + std::to_string(shown->line() + 1) + " kolumna " +
                                  std::to_string(shown->column() + 1) + "  UTF-8";

        const int room =
            term.size().wide - talcite::measured(left).wide - talcite::measured(right).wide;

        bar->text(left + std::string(room > 1 ? static_cast<std::size_t>(room) : 1, ' ') + right);
    };

    sheet.sized = naming;

    // Nasłuch wisi na kolumnie, nie na pasku: zdarzenie idzie bąbelkiem w górę od edytora,
    // a pasek jest jego rodzeństwem. Obserwator oddaje fałsz, żeby nie zasłonić go nikomu wyżej.
    (void)column->on<talcite::ui::ScrollEvent>([naming](talcite::ui::ScrollEvent&) {
        naming();

        return false;
    });

    const auto saying = [told, naming](std::string what) {
        *told = std::move(what);

        naming();
    };

    const auto where = std::make_shared<std::string>();

    // Zapis idzie przez pulę: plik otwiera `fs::open`, kawałki pisze `File::write`, a edytor
    // pierze dziennik dopiero po spełnieniu całości. Obietnicę trzeba trzymać — inaczej zginie
    // z tym domknięciem.
    const auto saving = std::make_shared<std::optional<talcite::Promise<void>>>();

    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Zapisz"), [shown, where, saying, saving] {
        if (where->empty()) {
            saying("nie ma dokąd zapisać");

            return;
        }

        if (shown->busy()) {
            saying("zapis trwa");

            return;
        }

        saying("zapisuję…");

        const std::filesystem::path onto = talcite::fs::from_string(*where);

        saving->emplace(
            talcite::fs::open(onto, talcite::fs::Mode::truncate)
                .here()
                .then([shown](talcite::fs::File file) {
                    const auto at = std::make_shared<std::uintmax_t>(0);

                    return shown->save([file, at](talcite::Bytes got) mutable {
                        // Pusty kawałek to koniec: nie ma co pisać, jest co domknąć.
                        if (got.empty()) {
                            file.close();

                            return talcite::Promise<void>{[](auto resolve, auto) { resolve(); }};
                        }

                        const std::uintmax_t from = *at;

                        *at += got.size();

                        return file.write(from, std::move(got)).then([](std::size_t) {});
                    });
                })
                .here()
                .then([where, saying] { saying("zapisano do " + *where); })
                .fail([saying](const std::exception& why) {
                    saying(std::string{"zapis się nie udał: "} + why.what());
                }));

        (*saving)->start();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Szukaj"), [dialogs, shown, saying] {
        dialogs->open(demo::closing(hunting(
            "Szukanie", false,
            [shown, saying](std::string what, std::string, talcite::Sift how) {
                const talcite::Comb comb{std::move(what), how};

                if (!comb.ready()) {
                    saying(comb.wrong());

                    return;
                }

                shown->focus();

                if (shown->find(comb, shown->cursor(), false).has_value())
                    saying({});
                else
                    saying("nie znaleziono");
            })));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f4, "Zamień"), [dialogs, shown, saying] {
        dialogs->open(demo::closing(hunting(
            "Zamiana", true,
            [shown, saying](std::string what, std::string with, talcite::Sift how) {
                const talcite::Comb comb{std::move(what), how};

                if (!comb.ready()) {
                    saying(comb.wrong());

                    return;
                }

                shown->focus();

                const std::size_t much = shown->replace_all(comb, with);

                saying(much > 0 ? "zamieniono: " + std::to_string(much)
                                : std::string{"nie znaleziono"});
            })));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Zawijaj"), [shown, naming] {
        shown->wrapped(!shown->wrapped());

        naming();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f7, "Cofnij"), [shown, saying] {
        shown->undo();

        saying({});
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f8, "Ponów"), [shown, saying] {
        shown->redo();

        saying({});
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjście"),
                      [&term] { term.stop(); });

    // Plik wchodzi w całości, bo edytor jest rezydentny — ale czyta go pula (`fs::read`), a edytor
    // dostaje bajty przy obrocie pętli; do tej pory jest pusty.
    std::optional<talcite::Promise<void>> opening;

    if (argc > 1) {
        const std::filesystem::path from = talcite::fs::from_string(argv[1]);

        *where = std::string{argv[1]} + ".new";

        opening.emplace(talcite::fs::read(from)
                            .here()
                            .then([shown, source, from](talcite::Bytes got) {
                                *source = talcite::fs::to_string(from);

                                shown->hold(std::move(got));
                            })
                            .fail([shown, source, from](const std::exception&) {
                                *source = "nie udało się otworzyć: " + talcite::fs::to_string(from);

                                shown->hold(lump(sample));
                            }));

        opening->start();
    } else {
        shown->hold(lump(sample));

        *where = "editor.new";
    }

    term.show(sheet);
    term.title("talcite — edytor tekstu");

    shown->focus();

    naming();

    term.run();

    return 0;
}
