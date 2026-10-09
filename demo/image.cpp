#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/image.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/mosaic.hpp"
#include "talcite/pane.hpp"
#include "talcite/pixels.hpp"
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

// **Bez pliku i bez wbudowanego GIF-a.** Dekodery mieszkają w talcite-adapters, a talcite nie
// linkuje przejściówek — więc demo pokazuje to, co widżet robi z obrazkami **robionymi w kodzie**:
// proporcję kratki, cięcia idące za krawędzią, przezroczystość przechodzącą przez tło ramki
// i różnicę między dwiema gęstościami siatki. Animację z pliku i prawdziwe pliki z dysku ogląda
// program sprawdzający przejściówkę (talcite-stb-check).
talcite::Pixels painted(int wide, int high, void (*fn)(double, double, std::uint8_t*)) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(wide) * high * 4);

    for (int y = 0; y < high; ++y)
        for (int x = 0; x < wide; ++x)
            fn((x + 0.5) / wide * 2 - 1, (y + 0.5) / high * 2 - 1,
               bytes.data() + (static_cast<std::size_t>(y) * wide + x) * 4);

    talcite::Bytes lump;

    lump.reserve(bytes.size());

    for (const std::uint8_t one : bytes) lump.push_back(std::byte{one});

    return talcite::Pixels{{wide, high}, {talcite::Still{std::move(lump), std::chrono::milliseconds{0}}}};
}

// Koło barw: odcień z kąta, nasycenie z promienia. Sprawdza naraz proporcję kratki — koło ma być
// kołem — i to, czy cięcia idą za krawędzią barwy, a nie za jasnością.
void wheeled(double x, double y, std::uint8_t* at) {
    const double r = std::sqrt(x * x + y * y);

    if (r > 1.0) {
        at[3] = 0;

        return;
    }

    const double turn = (std::atan2(y, x) / 3.14159265358979 + 1.0) * 3.0;
    const double part = turn - std::floor(turn);

    const double up = 255.0 * r;
    const double down = 255.0 * r * (1.0 - part);
    const double rise = 255.0 * r * part;
    const double rest = 255.0 * (1.0 - r);

    double one[3] = {0, 0, 0};

    switch (static_cast<int>(turn) % 6) {
    case 0:
        one[0] = up;
        one[1] = rise;
        break;
    case 1:
        one[0] = down;
        one[1] = up;
        break;
    case 2:
        one[1] = up;
        one[2] = rise;
        break;
    case 3:
        one[1] = down;
        one[2] = up;
        break;
    case 4:
        one[0] = rise;
        one[2] = up;
        break;
    default:
        one[0] = up;
        one[2] = down;
        break;
    }

    for (std::size_t band = 0; band < 3; ++band)
        at[band] = static_cast<std::uint8_t>(one[band] + rest);

    at[3] = 255;
}

// Pierścień z dziurą: przezroczystość widać wtedy, gdy przez środek prześwituje tło ramki.
void ringed(double x, double y, std::uint8_t* at) {
    const double r = std::sqrt(x * x + y * y);

    at[0] = 255;
    at[1] = 190;
    at[2] = 0;
    at[3] = r <= 1.0 && r >= 0.5 ? 255 : 0;
}

// Skos: najostrzejsza próba dla kształtów. Znaki blokowe mają na to ćwiartki i ósemki, sekstanty
// gęstszą siatkę — i to tu różnicę widać najlepiej.
void slanted(double x, double y, std::uint8_t* at) {
    const bool over = y < x;

    at[0] = over ? 250 : 20;
    at[1] = over ? 240 : 30;
    at[2] = over ? 210 : 90;
    at[3] = 255;
}

// Gradient: sprawdza uśrednianie. Przy uśrednianiu surowych bajtów środek wychodzi zauważalnie
// ciemniejszy, niż powinien.
void faded(double x, double y, std::uint8_t* at) {
    const double one = (x + 1.0) / 2.0;
    const double two = (y + 1.0) / 2.0;

    at[0] = static_cast<std::uint8_t>(255.0 * one);
    at[1] = static_cast<std::uint8_t>(255.0 * two);
    at[2] = static_cast<std::uint8_t>(255.0 * (1.0 - one));
    at[3] = 255;
}

std::string told(talcite::ui::Blocks kind) {
    return kind >= talcite::ui::Blocks::sextants ? "sekstanty (2x3)" : "znaki blokowe (8x8)";
}

}

int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — obrazy");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto said = std::make_shared<talcite::ui::Span>(std::string{});

    const auto shown = std::make_shared<talcite::ui::Image>();

    shown->hold(painted(240, 240, wheeled));

    // Pojemnik obejmuje **całą treść okna**, tak jak w pozostałych demach: kliknięcie gdziekolwiek
    // w ramce zabiera ognisko, a bez ogniska legenda nie dostałaby klawiszy.
    const auto pane = std::make_shared<talcite::ui::Pane>();

    column->children().add(said);
    column->children().add(shown);

    frame->children().add(column);
    pane->children().add(frame);
    legend->content(pane);

    // Co teraz widać — trzymane **osobno**, a nie wyłuskiwane z napisu na ekranie. Poprzednio
    // odtwarzałem je przez `substr` z tego, co sam przed chwilą wypisałem, i to była proteza:
    // napis jest dla człowieka, nie na stan programu.
    const auto subject = std::make_shared<std::string>("koło barw");

    // Życzenie aplikacji wobec terminala. Trzymamy je u siebie, bo `reconfigure` idzie przez pętlę
    // i wchodzi dopiero na granicy klatki — a podpis chcemy mieć od razu.
    const auto wish = std::make_shared<talcite::Setup>();

    // **Stan ruchu idzie na sam przód i wypisuje się zawsze.** Wcześniej stał na końcu długiego
    // napisu i tylko wtedy, gdy było co animować — więc F7 wyglądał na martwy dokładnie wtedy,
    // gdy najbardziej chciało się wiedzieć, czy działa.
    const auto naming = [said, shown, subject, wish] {
        // Napis mówi **stan przełącznika**, a nie to, czy akurat coś się rusza — bo tylko wtedy
        // F7 widać od pierwszej chwili. Liczba klatek dochodzi dopiero po pierwszym układzie, bo
        // GIF nie mówi jej w nagłówku, i to jest jedyne, co się tu pojawia z opóźnieniem.
        std::string more = shown->play() ? "ruch" : "stop";

        more += " · " + *subject;

        if (shown->frames() > 1) more += ", klatek " + std::to_string(shown->frames());

        more += " · " + told(wish->blocks.value_or(talcite::ui::Blocks::basic));

        said->text(more);
    };

    const auto picking = [shown, naming, subject](const char* what, talcite::Pixels given) {
        return [shown, naming, subject, what, given = std::move(given)] {
            shown->hold(given);

            *subject = what;

            naming();
        };
    };

    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Koło"),
                      picking("koło barw", painted(240, 240, wheeled)));

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Pierść"),
                      picking("pierścień z dziurą", painted(240, 240, ringed)));

    (void)demo::doing(legend->add(talcite::ui::Code::f4, "Skos"),
                      picking("skos", painted(240, 240, slanted)));

    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Gradient"),
                      picking("gradient", painted(240, 240, faded)));

    // **Sekstantów nie włączamy z pomiaru.** Sonda mówi tylko, czy znak zajmuje jedną kratkę,
    // a nie czy font go ma — więc życzenie stawia tu użytkownik, po jednym naciśnięciu i jednym
    // spojrzeniu na ekran. Windows Terminal podmienia czcionkę i pokaże je nawet bez Cascadii.
    //
    // Widżet nie ma już czego przestawiać: gęstość leży na korzeniu, a stawia ją tam `Term`.
    // Klawisz mówi więc **terminalowi**, a obrazek dowiaduje się o tym przy najbliższej klatce —
    // sam, przez porównanie w `spread()`. To jedyne wywołanie `reconfigure` w całym projekcie.
    (void)demo::doing(legend->add(talcite::ui::Code::f6, "Siatka"), [&term, wish, naming] {
        wish->blocks = wish->blocks.value_or(talcite::ui::Blocks::basic) <
                               talcite::ui::Blocks::sextants
                           ? talcite::ui::Blocks::sextants
                           : talcite::ui::Blocks::basic;

        term.reconfigure(*wish);

        naming();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f7, "Ruch"), [shown, naming] {
        shown->play(!shown->play());

        naming();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(legend);

    pane->focus();

    // **Animacja chodzi sama**, gdy jest co animować: widżet mówi korzeniowi, kiedy najpóźniej
    // chce być obudzony, a `Term` wkłada to do swojego ogranicznika — F7 tylko zatrzymuje
    // i puszcza. Obrazki z kodu są nieruchome, więc tu F7 pokazuje jedynie stan przełącznika;
    // ruch z pliku ogląda talcite-stb-check.
    naming();

    term.show(sheet);
    term.title("talcite — obrazy");

    term.run();

    return 0;
}
