#include "proving.hpp"

#include <cstdlib>
#include <string>
#include <utility>

#include "talcite/compatibilities.hpp"

using namespace talcite;

namespace {

// Terminal nagrany. Nadpisujemy szew rozmowy, więc rozbiór odpowiedzi sprawdza się bez
// żadnego terminala i na każdym systemie.
class Taped : public Compatibilities {
public:
    explicit Taped(std::string said) : said_(std::move(said)) {}

    [[nodiscard]] const std::string& asked() const { return asked_; }

    [[nodiscard]] int rounds() const { return rounds_; }

protected:
    [[nodiscard]] bool lively() const override { return true; }

    void tell(std::string_view bytes) const override { asked_ += bytes; }

    [[nodiscard]] std::string hear(std::chrono::milliseconds) const override {
        ++rounds_;

        return std::exchange(said_, std::string{});
    }

    [[nodiscard]] bool painting() const override { return false; }

private:
    mutable std::string asked_;
    mutable std::string said_;
    mutable int rounds_ = 0;
};

// Terminal nieodpowiadający — ani jednego bajtu, także na wartownika. Za konsolę odpowiada sam,
// zamiast pytać tę prawdziwą: inaczej wynik zależałby od tego, z czego uruchomiono program.
class Mute : public Compatibilities {
public:
    explicit Mute(bool painting = false) : painting_(painting) {}

protected:
    [[nodiscard]] bool lively() const override { return true; }

    void tell(std::string_view) const override {}

    [[nodiscard]] std::string hear(std::chrono::milliseconds) const override { return {}; }

    [[nodiscard]] bool painting() const override { return painting_; }

private:
    bool painting_ = false;
};

// Terminal, z którym nie ma jak rozmawiać — przekierowanie do potoku.
class Apart : public Compatibilities {
protected:
    [[nodiscard]] bool lively() const override { return false; }

    void tell(std::string_view) const override { said_ = true; }

    [[nodiscard]] std::string hear(std::chrono::milliseconds) const override { return {}; }

public:
    [[nodiscard]] bool said() const { return said_; }

private:
    mutable bool said_ = false;
};

const std::string modern =
    "\x1b[>0;280;0c"
    "\x1b[?1006;2$y"
    "\x1b[?1000;2$y"
    "\x1b[?2004;2$y"
    "\x1b[?1004;2$y"
    "\x1b[?2026;2$y"
    "\x1b[?1049;2$y"
    "\x1bP1+r5463=787465726d2d323536636f6c6f72\x1b\\"
    "\x1b]11;rgb:0000/0000/0000\x1b\\"
    "\x1b[1;3R"
    "\x1b[1;2R"
    "\x1b[1;3R"
    "\x1b[?62;1;2;6;9;15;22;29c";

}

TRIAL("Compatibilities: pytania kończą się wartownikiem") {
    const Taped one{modern};

    (void)one.colors();

    const std::string& asked = one.asked();

    ought(!asked.empty());
    ought(asked.rfind("\x1b[c") == asked.size() - 3);
}

TRIAL("Compatibilities: nowoczesny terminal oddaje wszystko") {
    const Taped one{modern};

    same(static_cast<int>(one.mouse()), static_cast<int>(Mouse::sgr));
    same(static_cast<int>(one.screen()), static_cast<int>(Screen::apart));
    ought(one.pasting());
    ought(one.focusing());
    ought(one.syncing());
    same(static_cast<int>(one.shade()), static_cast<int>(Shade::dark));
    same(one.version(), 280);
    same(one.named(), std::string{"xterm"});
}

TRIAL("Compatibilities: pomiary szerokości wracają po kolei") {
    const Taped one{modern};

    same(one.wide(), 2);
    same(one.varied(), 1);
    same(one.joined(), 2);
}

TRIAL("Compatibilities: XTGETTCAP podnosi kolor do pełnego") {
    const Taped one{modern};

    same(static_cast<int>(one.colors()), static_cast<int>(Colors::rgb));
}

TRIAL("Compatibilities: nieznany tryb zostaje nieznany") {
    const Taped one{
        "\x1b[?1006;0$y"
        "\x1b[?2004;0$y"
        "\x1b[?2026;0$y"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.mouse()), static_cast<int>(Mouse::basic));
    ought(!one.pasting());
    ought(!one.syncing());
}

TRIAL("Compatibilities: jasne tło rozpoznane") {
    const Taped one{
        "\x1b]11;rgb:ffff/ffff/ffff\x1b\\"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.shade()), static_cast<int>(Shade::light));
}

TRIAL("Compatibilities: wartownik kończy czekanie od razu") {
    const Taped one{"\x1b[?62;1c"};

    (void)one.colors();

    same(one.rounds(), 1);
}

TRIAL("Compatibilities: milczący terminal zostaje przy podłodze") {
    const Mute one;

    same(static_cast<int>(one.screen()), static_cast<int>(Screen::older));
    ought(!one.pasting());
    ought(!one.syncing());
    same(one.wide(), 1);
}

TRIAL("Compatibilities: bez terminala nie wysyłamy ani bajtu") {
    const Apart one;

    (void)one.colors();

    ought(!one.said());
}

TRIAL("Compatibilities: pytamy raz, choćby pytań było wiele") {
    const Taped one{modern};

    (void)one.colors();
    (void)one.mouse();
    (void)one.pasting();
    (void)one.wide();

    same(one.rounds(), 1);
}

TRIAL("Compatibilities: cudze bajty nie giną") {
    const Taped one{
        "abc"
        "\x1b[?2004;2$y"
        "q"
        "\x1b[?62;1c"};

    ought(one.pasting());
    same(one.rest(), std::string{"abcq"});
}

TRIAL("Compatibilities: odpowiedź poszatkowana składa się w całość") {
    class Torn : public Compatibilities {
    public:
        explicit Torn(std::string said) : said_(std::move(said)) {}

    protected:
        [[nodiscard]] bool lively() const override { return true; }

        void tell(std::string_view) const override {}

        [[nodiscard]] std::string hear(std::chrono::milliseconds) const override {
            if (said_.empty()) return {};

            std::string made = said_.substr(0, 3);
            said_.erase(0, 3);

            return made;
        }

    private:
        mutable std::string said_;
    };

    const Torn one{modern};

    same(static_cast<int>(one.mouse()), static_cast<int>(Mouse::sgr));
    ought(one.pasting());
    same(one.wide(), 2);
}

TRIAL("Compatibilities: krótki zapis koloru tła też działa") {
    const Taped one{
        "\x1b]11;rgb:f/f/f\x1b\\"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.shade()), static_cast<int>(Shade::light));
}

TRIAL("Compatibilities: sam błękit to ciemne tło, choć składowa wysoka") {
    const Taped one{
        "\x1b]11;rgb:0000/0000/ffff\x1b\\"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.shade()), static_cast<int>(Shade::dark));
}

TRIAL("Compatibilities: kolor tła zakończony dzwonkiem też się liczy") {
    const Taped one{
        "\x1b]11;rgb:ffff/ffff/ffff\x07"
        "\x1b[?62;1c"};

    same(static_cast<int>(one.shade()), static_cast<int>(Shade::light));
}

// Ogon, który w chwili końca badania jest niedokończony, może być równie dobrze początkiem
// naciśniętego klawisza. Wyrzucenie go gubiłoby klawisz, więc idzie do cudzych bajtów.
TRIAL("Compatibilities: niedokończony ogon nie ginie") {
    const Taped one{
        "\x1b[?2004;2$y"
        "\x1b[?62;1c"
        "\x1b[A"};

    ought(one.pasting());
    same(one.rest(), std::string{"\x1b[A"});
}

TRIAL("Compatibilities: tmux i screen rozpoznają się po rodzinie") {
    const Taped one{"\x1b[>84;0;0c\x1b[?62;1c"};
    const Taped two{"\x1b[>83;40500;0c\x1b[?62;1c"};

    same(one.named(), std::string{"tmux"});
    same(two.named(), std::string{"screen"});
}

TRIAL("Compatibilities: klawisze wciśnięte w trakcie startu nie giną") {
    const Taped one{
        "\x1b[?2004;2$y"
        "\x1b[A"
        "\x1b[?62;1c"
        "q"};

    ought(one.pasting());
    same(one.rest(), std::string{"\x1b[A" "q"});
}

TRIAL("Compatibilities: nieznana odpowiedź OSC też wraca w całości") {
    const Taped one{
        "\x1b]10;rgb:1111/1111/1111\x1b\\"
        "\x1b[?62;1c"};

    same(one.rest(), std::string{"\x1b]10;rgb:1111/1111/1111\x1b\\"});
}

namespace {

// Podłoga czyta środowisko, więc żeby ją sprawdzić, trzeba je na chwilę przestawić. Nazwy
// wywołań różnią się między systemami, a pusta wartość na Windows znaczy skreślenie wpisu.
void putting(const char* what, const char* given) {
#ifdef _WIN32
    _putenv_s(what, given != nullptr ? given : "");
#else
    if (given != nullptr) ::setenv(what, given, 1);
    else ::unsetenv(what);
#endif
}

class Swapped {
public:
    Swapped(const char* what, const char* given) : what_(what) {
        const char* told = std::getenv(what);

        if (told != nullptr) {
            was_ = told;
            held_ = true;
        }

        putting(what, given);
    }

    ~Swapped() { putting(what_, held_ ? was_.c_str() : nullptr); }

    Swapped(const Swapped&) = delete;
    Swapped& operator=(const Swapped&) = delete;
    Swapped(Swapped&&) = delete;
    Swapped& operator=(Swapped&&) = delete;

private:
    const char* what_ = nullptr;
    std::string was_;
    bool held_ = false;
};

}

TRIAL("Compatibilities: NO_COLOR gasi kolor mimo odpowiedzi terminala") {
    const Swapped one{"NO_COLOR", "1"};
    const Swapped two{"COLORTERM", "truecolor"};

    const Taped told{modern};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::mono));
}

TRIAL("Compatibilities: COLORTERM podnosi do pełnej palety") {
    const Swapped one{"NO_COLOR", nullptr};
    const Swapped two{"COLORTERM", "truecolor"};
    const Swapped three{"TERM", "xterm"};

    const Mute told;

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

TRIAL("Compatibilities: TERM z 256 daje paletę indeksowaną") {
    const Swapped one{"NO_COLOR", nullptr};
    const Swapped two{"COLORTERM", nullptr};
    const Swapped three{"TERM", "xterm-256color"};

    const Mute told;

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::palette));
}

TRIAL("Compatibilities: TERM dumb znaczy nic") {
    const Swapped one{"NO_COLOR", nullptr};
    const Swapped two{"COLORTERM", nullptr};
    const Swapped three{"WT_SESSION", nullptr};
    const Swapped four{"TERM", "dumb"};

    const Mute told;

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::mono));
    same(static_cast<int>(told.screen()), static_cast<int>(Screen::none));
    same(static_cast<int>(told.mouse()), static_cast<int>(Mouse::none));
    ought(!told.titling());
}

// Na tej drodze **decyduje konsola, nie TERM** — i tylko tutaj, bo wszystkie wcześniejsze gałęzie
// (NO_COLOR, COLORTERM, 256, dumb) kończą się przed pytaniem konsoli. Odpowiedź konsoli podstawia
// atrapa, więc obie strony tej decyzji sprawdzają się na każdym systemie, a wynik nie zależy od
// tego, z czego uruchomiono program.
TRIAL("Compatibilities: zwykły TERM przy konsoli bez sekwencji daje szesnaście kolorów") {
    const Swapped one{"NO_COLOR", nullptr};
    const Swapped two{"COLORTERM", nullptr};
    const Swapped three{"WT_SESSION", nullptr};
    const Swapped four{"TERM", "vt220"};

    const Mute told{false};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::ansi));
}

TRIAL("Compatibilities: zwykły TERM przy konsoli z sekwencjami daje pełną paletę") {
    const Swapped one{"NO_COLOR", nullptr};
    const Swapped two{"COLORTERM", nullptr};
    const Swapped three{"WT_SESSION", nullptr};
    const Swapped four{"TERM", "vt220"};

    const Mute told{true};

    same(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

// --- Obrazy -------------------------------------------------------------------------------------
//
// Sixela nie musimy o nic pytać: pierwotne DA wysyłamy jako wartownika sondy, a czwórka na jego
// liście znaczy „umiem sixel". Kitty odpowiada w APC. Rozmiar kratki w pikselach przychodzi z `16t`.

namespace {

// Pięć pomiarów szerokości po kolei: szeroki znak, znacznik, złączenie, sekstant, oktant.
std::string taped(const char* wides, const char* da, const char* more = "") {
    std::string made;

    made += "\x1b[>0;280;0c";
    made += more;

    for (const char* at = wides; *at != '\0'; ++at) {
        made += "\x1b[1;";
        made += static_cast<char>('0' + (*at - '0') + 1);
        made += "R";
    }

    made += da;

    return made;
}

const char* const plain = "\x1b[?62;1;2;6;22c";
const char* const drawing = "\x1b[?62;1;2;4;6;22c";

}

TRIAL("Compatibilities: sixel poznaje się z pierwotnego DA") {
    const Taped one{taped("21211", drawing)};

    same(static_cast<int>(one.graphics()), static_cast<int>(Graphics::sixel));
}

TRIAL("Compatibilities: bez czwórki w DA nie ma sixela") {
    const Taped one{taped("21211", plain)};

    same(static_cast<int>(one.graphics()), static_cast<int>(Graphics::none));
}

// Kitty jest lepsze od sixela — pełna barwa zamiast palety — więc gdy terminal umie oba, wygrywa.
TRIAL("Compatibilities: kitty wygrywa z sixelem") {
    const Taped one{taped("21211", drawing, "\x1b_Gi=31;OK\x1b\\")};

    same(static_cast<int>(one.graphics()), static_cast<int>(Graphics::kitty));
}

TRIAL("Compatibilities: odpowiedź kitty nie wraca jako naciśnięte klawisze") {
    const Taped one{taped("21211", plain, "\x1b_Gi=31;OK\x1b\\")};

    same(static_cast<int>(one.graphics()), static_cast<int>(Graphics::kitty));
    same(one.rest(), std::string{});
}

TRIAL("Compatibilities: rozmiar kratki przychodzi z 16t") {
    const Taped one{taped("21211", plain, "\x1b[6;20;10t")};

    same(one.cell().wide, 10);
    same(one.cell().high, 20);
}

// Zero znaczy „nie wiadomo", a nie „zero pikseli" — i wołający ma prawo to odróżnić, bo od tego
// zależy, czy zgadywać proporcję kratki.
TRIAL("Compatibilities: nieznany rozmiar kratki to zero") {
    const Taped one{taped("21211", plain)};

    same(one.cell().wide, 0);
    same(one.cell().high, 0);
}

TRIAL("Compatibilities: cudzy raport okna nie jest rozmiarem kratki") {
    const Taped one{taped("21211", plain, "\x1b[8;24;80t")};

    same(one.cell().wide, 0);
}

// --- Znaki blokowe ------------------------------------------------------------------------------
//
// Pomiar odpowiada wyłącznie na pytanie „czy ten znak zajmuje jedną kratkę". Czy font go ma, nie
// da się zmierzyć — brakujący glif rysuje się jako pusty prostokąt i zajmuje tyle samo miejsca.
// Dlatego `blocks()` oddaje **sufit**, a nie wybór: mówi, czego terminal jeszcze nie rozsypie.
// Wyboru dokonuje `Term`, biorąc mniejszą z dwóch — życzenia i sufitu.

TRIAL("Blocks: oba znaki równe kratce podnoszą sufit do oktantów") {
    const Taped one{taped("21211", plain)};

    same(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::octants));
}

TRIAL("Blocks: rozjechany sekstant zbija sufit do znaków blokowych") {
    const Taped one{taped("21221", plain)};

    same(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::basic));
}

TRIAL("Blocks: rozjechany oktant zbija sufit do sekstantów") {
    const Taped one{taped("21212", plain)};

    same(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::sextants));
}

// Milczący terminal niczego nie obalił, więc sufit zostaje wysoko. Brak pomiaru to nie to samo,
// co pomiar mówiący „nie".
TRIAL("Blocks: bez pomiaru sufit zostaje najwyższy") {
    const Mute one;

    same(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::octants));
}

TRIAL("Compatibilities: sonda pyta o kratkę i o kitty, a wartownik dalej jest ostatni") {
    const Taped one{taped("21211", plain)};

    (void)one.graphics();

    const std::string& asked = one.asked();

    ought(asked.find("\x1b[16t") != std::string::npos);
    ought(asked.find("\x1b_Gi=31") != std::string::npos);
    ought(asked.rfind("\x1b[c") == asked.size() - 3);
}

