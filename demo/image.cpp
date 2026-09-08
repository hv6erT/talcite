#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/image.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legendwindow.hpp"
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
    bool react(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

private:
    talcite::Term& term_;
};

// **GIF wbudowany w program.** Demo ma pokazywać dekoder i animację, a nie sprawność
// w podawaniu ścieżek — więc jeden prawdziwy plik leży tu na stałe i uruchomienie bez żadnego
// argumentu od razu coś rusza. Trzy kilobajty; zrobiony w całości na tę potrzebę, więc nie ciągnie
// za sobą niczyich praw.
//
// 48 na 32, szesnaście klatek, sześć barw na przezroczystym tle — przez dziurę w środku widać tło
// ramki, a to jest przy okazji najkrótszy dowód, że przezroczystość przechodzi całą drogę.
constexpr std::string_view packed =
    "47494638396130002000820000000000ff2626ffff2626ff2626ffff2626ffff26ff00000021ff0b4e455453434150"
    "45"
    "322e30030100000021f90409080000002c00000000300020000008ab0001081c48b0a0c18308132a5cc8b0a1c38710"
    "23"
    "4a9c08b18045870632523c68b123c38c203712ec485221c893220592f498f064c8942b2fb6746920a5ca920b5dda1c"
    "c8"
    "f2a3c69d40830a5548a0e8d0004819165d1a14a953a24b99da744a356154a929a93e4578d5e854ad01a062fdba55ec"
    "d1"
    "a443d3261cc0b6a180b742d9ca5df8b62e50b97813d6ddbb13ef5c847beddaf4db1670600177f32a0c1cb7305db86a"
    "23"
    "4b9e4cb932c1800021f90409080000002c0600030024001a0082000000ff2626ffff2626ff2626ffff2626ffff26ff"
    "00"
    "0000089f0001081c489060818305132a5c98f0a0438610233a9c18b162c38908191ad8687120c68c0a378aec2810a3"
    "46"
    "9123071258c9f0e149940654ae6449b2204c8e0066eaac6913a54c9d0478f68cf973a7d088408f5a9ca9b469d30050"
    "9d"
    "42844a55aa42aa58090ed8ea146b55815bc33ef51a006c58b14abd0e3c8b76a180b70cbf9a65cbf0addd9a74dddabd"
    "4b"
    "b2adc2bd7cad0e040c573041c08613068e18100021f90409080000002c0500030026001a0082000000ff2626ffff26"
    "26"
    "ff2626ffff2626ffff26ff000000089f0001081c48b0a0410005121e5cc8b061c2870d17129818f1a1c58805276a74"
    "68"
    "512146811a4332ece8f163c88d232f7e04799240c5922b019c8cb9d0804d8914691ab4c953674c9e407d7e04da5368"
    "44"
    "a237010c586af42051814ba336dd9934aad5a90cad4ac57a502b53ae5daf4e0d4076e1d6a664d3fa14c03662dab734"
    "d9"
    "ca6df856ed4ab97819d62d7b176f5bbd7063fafd0b38c0dabc6017ce6d1a100021f90409080000002c050003002600"
    "1a"
    "0082000000ff2626ffff2626ff2626ffff2626ffff26ff00000008a20001081c481000818305132a5cb8f0a0438610"
    "23"
    "0a74483162818b120b527cc8f0a2c78c0337225ce8b124c889153b96c478d2e048952b5bb68cb970804d99053fd6b4"
    "79"
    "1367469e407d4a04ca53684203480512ed695420d2a74a833675fa142a80a253a956359055e1d6a45d8f566528a06c"
    "53"
    "ab0acbaa0d9b50addb8801e2ca74bb9661dcbb2de99a5d78b76fdeb776fbcafd2b00aee0006c0b0a4e9c102fc48000"
    "21"
    "f90409080000002c0700030022001b0082000000ff2626ffff2626ff2626ffff2626ffff26ff000000089d0001081c"
    "48"
    "b020818305132a5c48f0a0438610193a9c18b162c389082d5ac4985161818f040788944871e1c7930245aa244980e1"
    "c9"
    "972a636a4cf8f264cc953309d6fc7873644e9d2f53cafc5910e5409c44932a5dcab4e94201509d1a983a10aa55a653"
    "b3"
    "02b0ca5569d6af5caf26fd9a356cd4b1640d6cedca30805b8664ab9e5de8b62e5caa33ebea753a50af5dbe00fcbe05"
    "1c"
    "782f61817f2d06040021f90409080000002c0500030026001a0082000000ff2626ffff2626ff2626ffff2626ffff26"
    "ff"
    "000000089f0001081c48b0a0c18104121e5cc8b061c2870d170e9818f1a1c58805276a7468512146811a4332ece8f1"
    "63"
    "c88d232f7e04797240c5922b019c8c497320ca9a030be8c44953a74f9e1f7d0a1528a0285083427f165d7a9460529d"
    "4b"
    "a336cd39342ad3a9027f02b06a14eb41ab5e195e056aa06cd88565d3e20cc03662dab734d9ca6df856ed4ab97819d6"
    "35"
    "7b176f5bbd7063fafd0bd8c0dabc670dce9d1a100021f90409080000002c0500030026001a0082000000ff2626ffff"
    "26"
    "26ff2626ffff2626ffff26ff00000008a00001081c4870e0808305132a5cb8f0a0438610230a74483122818b120b52"
    "7c"
    "c8f0a2c78c06370ee8e8f12348001b219634799223c9950905c83cb91066419938692a643910a74f9d207de6042a51"
    "e8"
    "4ca2457f224d58a029c1a14b09369d1a15e2d4ab55175ea59a95e9d602090388adba35acd8b151b9123ccb36a281b7"
    "3a"
    "d99e85f8b62e4db96815d6dd7bb72dc3bd76fb06700bd84057bd7c0f23362c31200021f90409080000002c06000300"
    "24"
    "001a0082000000ff2626ffff2626ff2626ffff2626ffff26ff000000089d0001081c48b020c101080d2a5cc85020c2"
    "87"
    "0d23327c4851224101181b5284681100c68f13370ee8f8b164488e164b823cc99080cb8b2a057454e8b226cc953309"
    "d6"
    "dc7933a7c19d367d4604fa52e8509e4625061518a06952894da33e6518b5ea548555a53e2dc0956056a749b98af56a"
    "35"
    "acd8b103b52a34c0b6e159b416d9ca65f8b66b5cb97317be9d8937af5ebb1dfbb6bd6ab02fe1857e7306040021f904"
    "09"
    "080000002c050004002700180082000000ff2626ffff2626ff2626ffff2626ffff26ff00000008a10001081c385080"
    "41"
    "8208090e5898b0614283101d0e5c4851a24388182552dc68f121c6830e3756ec48f023c8862219922c99d1a2c8951e"
    "05"
    "ac1c09b3a6cd9b380512d8493080cf9c12770a15e8b328d0844293165d7a946052a14b8d36d5f99440d49f53015425"
    "00"
    "206ad6814f074afd4a952bd9a906d2c22cc01667dab724d9cab5f9b6ae45b9786bd6852b11ef5c987bd5f6f55b80ae"
    "dd"
    "8e7edd0a8edb5662400021f90409080000002c0600030024001a0082000000ff2626ffff2626ff2626ffff2626ffff"
    "26"
    "ff000000089f0001081c4890a0808305132a5c98f0a0438610233a9c18b162c38908190ed8687120c68c0a378aec28"
    "10"
    "a3469123070658c9f0e149940354ae6449b2204c8e0066eaac6913a54c9d0178f68cf973a7d088408f5a9ca9b46953"
    "02"
    "509d42844a55aa42aa58091ad8ea146b55815bc33ef54a006c58b14abd0e3c8b766181b70cbf9a65cbf0addd9a74dd"
    "da"
    "bd4bb2adc2bd7cad0e040c573041c08613068e18100021f90409080000002c0500030026001a0082000000ff2626ff"
    "ff"
    "2626ff2626ffff2626ffff26ff000000089f0001081c48b0a0410002121e5cc8b061c2870d17069818f1a1c5880527"
    "6a"
    "7468512146811a4332ece8f163c88d232f7e04793240c5922b019c8cb970804d8914691ab4c953674c9e407d7e04da"
    "53"
    "6844a2370118586af42051814ba336dd9934aad5a90cad4ac57a502b53ae5daf4e254076e1d6a664d3fa2cc03662da"
    "b7"
    "34d9ca6df856ed4ab97819d62d7b176f5bbd7063fafd0b98c0dabc6017ce6d1a100021f90409080000002c05000300"
    "26"
    "001a0082000000ff2626ffff2626ff2626ffff2626ffff26ff00000008a20001081c481040808305132a5cb8f0a043"
    "86"
    "10230a744831a2808b120b527cc8f0a2c78c0337225ce8b124c889153b96c478d2e048952b5bb68cb9d0804d99053f"
    "d6"
    "b4791367469e407d4a04ca5368c201480512ed695420d2a74a833675fa142a80a253a9561d9055e1d6a45d8f566558"
    "a0"
    "6c53ab0acbaa0d9b50addb8804e2ca74bb9661dcbb2de99a5d78b76fdeb776fbcafd5b00ae60026c0b0a4e9c102fc4"
    "80"
    "0021f90409080000002c0700030022001b0082000000ff2626ffff2626ff2626ffff2626ffff26ff000000089c0001"
    "08"
    "1c48b060808305132a5c48f0a0438610193a9c18b162c389082d5ac49851a1808f040d88944871e1c7930245aa2419"
    "80"
    "e1c9972a636a4cf8f264cc953309d6fc7873644e9d2f53cafc5910e5409c44932a5dcab429800150091698da14aa55"
    "81"
    "53b32eb5ca35ab57a55cad7ad59a342cd4b154c172c5fa952181b710af0e24bbf0ad5da704edeac52b50ef5dbe7ee1"
    "f2"
    "05e077f0c0bf1603020021f90409080000002c0500030026001a0082000000ff2626ffff2626ff2626ffff2626ffff"
    "26"
    "ff000000089f0001081c48b0a0c18101121e5cc8b061c2870d171a9818f1a1c58805276a7468512146811a4332ece8"
    "f1"
    "63c88d232f7e0479d240c5922b019c8c497320ca9a0305e8c44953a74f9e1f7d0a1558a0285083427f165d7a946052"
    "9d"
    "4ba336cd39342ad3a9027f02b06a14eb41ab5e195e053aa06cd88565d3e224c03662dab734d9ca6df856ed4ab97819"
    "d6"
    "357b176f5bbd7063fafd0b78c0dabc670dce9d1a100021f90409080000002c0500030026001a0082000000ff2626ff"
    "ff"
    "2626ff2626ffff2626ffff26ff00000008a00001081c4870a0818305132a5cb8f0a0438610230a74483162808b120b"
    "52"
    "7cc8f0a2c78c06371ae8e8f12348001b219634799223c995090bc83cb91066419938692a643910a74f9d207de6042a"
    "51"
    "e84ca2457f224d28a029c1a14b09369d1a15e2d4ab55175ea59a95e95601090988adba35acd8b151b9123ccb36e280"
    "b7"
    "3ad99e85f8b62e4db96815d6dd7bb72dc3bd76fb12700b784057bd7c0f23362c31200021f90409080000002c060003"
    "00"
    "24001a0082000000ff2626ffff2626ff2626ffff2626ffff26ff000000089d0001081c48b0204103080d2a5cc85020"
    "c2"
    "870d23327c485122c102181b5284681100c68f13371ae8f8b164488e164b823cc93080cb8b2a0b7454e8b226cc9533"
    "09"
    "d6dc7933a7c19d367d4604fa52e8509e4625061548a06952894da33e6518b5ea548555a53e15c0956056a749b98af5"
    "6a"
    "35acd8b103b52a1cc0b6e159b416d9ca65f8b66b5cb97317be9d8937af5ebb1dfbb6bd6ab02fe1857e730604003b";

talcite::Bytes unhexed(std::string_view hex) {
    talcite::Bytes made;

    made.reserve(hex.size() / 2);

    const auto value = [](char one) { return one <= '9' ? one - '0' : one - 'a' + 10; };

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
        made.push_back(static_cast<std::byte>(value(hex[at]) * 16 + value(hex[at + 1])));

    return made;
}

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

// Plik wczytujemy zwykłym strumieniem, bo `talcite::fs` ciągnęłoby tu za sobą pętlę i obietnice,
// a demo ma pokazywać dekoder, nie drogę do dysku.
talcite::Bytes slurped(const char* where) {
    std::ifstream in{where, std::ios::binary};

    const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    talcite::Bytes made;

    made.reserve(raw.size());

    for (const char one : raw)
        made.push_back(static_cast<std::byte>(static_cast<unsigned char>(one)));

    return made;
}

std::string named(talcite::Picture what) {
    switch (what) {
    case talcite::Picture::gif:
        return "GIF";
    case talcite::Picture::png:
        return "PNG";
    case talcite::Picture::jpeg:
        return "JPEG";
    case talcite::Picture::bmp:
        return "BMP";
    default:
        return "nieznany format";
    }
}

std::string told(talcite::ui::Blocks kind) {
    return kind >= talcite::ui::Blocks::sextants ? "sekstanty (2x3)" : "znaki blokowe (8x8)";
}

}

// Bez argumentu — obrazki robione w kodzie: proporcja kratki, cięcia idące za krawędzią,
// przezroczystość przechodząca przez tło ramki i różnica między dwiema gęstościami siatki.
// Z argumentem — **prawdziwy plik z dysku**, przepuszczony przez dekoder.
int main(int argc, char** argv) {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::LegendWindow>();
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
    legend->body()->children().add(pane);

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

    (void)demo::doing(legend->add(talcite::ui::Code::f8, "Wbudow"), [shown, naming, subject] {
        shown->hold(unhexed(packed));

        *subject = "wbudowany GIF";

        naming();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(legend);

    pane->focus();

    // **Bez argumentu rusza wbudowany GIF.** Demo ma pokazywać dekoder i animację od pierwszej
    // sekundy, bez podawania czegokolwiek — koło barw i reszta obrazków robionych w kodzie siedzą
    // pod F2 do F5.
    //
    // **Animacja chodzi sama.** Widżet mówi korzeniowi, kiedy najpóźniej chce być obudzony,
    // a `Term` wkłada to do swojego ogranicznika — F7 tylko zatrzymuje i puszcza.
    shown->hold(unhexed(packed));

    *subject = "wbudowany GIF";

    if (argc > 1) {
        const talcite::Bytes raw = slurped(argv[1]);

        if (raw.empty()) {
            // **Nieudane wczytanie nie kasuje tego, co już jest** — i mówi wprost, czego szukało.
            // Poprzednio zostawiało puste pole, a komunikat ginął w środku zwykłego napisu.
            *subject = std::string{"nie udało się wczytać: "} + argv[1];
        } else {
            shown->hold(raw);

            if (shown->held())
                *subject = named(shown->kind()) + " " + std::to_string(shown->shape().wide) + "x" +
                           std::to_string(shown->shape().high);
            else {
                *subject = shown->kind() == talcite::Picture::none
                               ? "to nie jest obrazek"
                               : named(shown->kind()) + " — tego jeszcze nie czytamy";

                shown->hold(unhexed(packed));
            }
        }
    }

    naming();

    term.onto(sheet);
    term.title("talcite — obrazy");

    term.run();

    return 0;
}
