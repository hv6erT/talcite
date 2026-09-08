#include "talcite/image.hpp"

#include <algorithm>
#include <utility>

#include "talcite/root.hpp"
#include "talcite/skin.hpp"

namespace talcite::ui {

Image::Image() { role("talcite.image"); }

namespace {

// **Gęstość siatki i wymiar kratki nie należą do widżetu.** Są faktem o powierzchni, na której
// rysujemy, więc leżą na korzeniu, a `Image` sięga po nie tak samo, jak sięga po skórkę. Poza
// drzewem zostaje to, co przy milczącym terminalu: znaki blokowe i „nie wiadomo".
[[nodiscard]] Blocks grained(const Node& one) {
    const Root* top = one.root();

    return top != nullptr ? top->blocks() : Blocks::basic;
}

[[nodiscard]] Size celled(const Node& one) {
    const Root* top = one.root();

    return top != nullptr ? top->cell() : Size{};
}

// Klatka bez opóźnienia to nie animacja, tylko obrazek. Dekoder GIF-a nigdy takich nie wypuszcza —
// najkrótsze podnosi do stu milisekund — ale obrazek zrobiony w kodzie bywa złożony z klatek
// o zerowym czasie i wtedy „która klatka teraz" nie ma odpowiedzi.
[[nodiscard]] std::chrono::milliseconds summed(const Pixels& one,
                                               std::vector<std::chrono::milliseconds>& upto) {
    upto.clear();
    upto.reserve(one.stills().size());

    std::chrono::milliseconds much{0};

    for (const Still& said : one.stills()) {
        much += said.after > std::chrono::milliseconds{0} ? said.after
                                                          : std::chrono::milliseconds{0};

        upto.push_back(much);
    }

    return much;
}

}

// **Trzymamy plik, nie obrazek.** To jest lekcja z `frk::Animation` posunięta do końca: tam klatki
// leżały skompresowane i rozwijała się jedna naraz, tu leży cały plik, a w pamięci stoją klatki
// **już w rozdzielczości kratek**. Pełnej rozdzielczości nie ma nigdy: dekoder podaje klatkę
// postacią strumieniową, widżet ją zmniejsza i porzuca.
//
// Zmierzone na GIF-ie 480x270 o sześćdziesięciu klatkach: pełne klatki to 31,1 MB, te same
// w rozdzielczości stu kratek — 4,32 MB. Płacimy przeliczeniem przy zmianie rozmiaru okna, około
// stu milisekund dla tak ciężkiego pliku i kilku dla zwykłego.
void Image::hold(Bytes raw) {
    raw_ = std::move(raw);
    whole_ = {};
    kind_ = guessed(raw_);
    shape_ = {};
    frames_ = 0;

    // Rozmiar musimy znać **przed** malowaniem, bo pyta o niego układ. Rozpakowujemy więc jedną
    // klatkę i o niej zapominamy — przy 480x270 to jest niecała milisekunda.
    decoded(raw_, [this](const Bytes&, Size given, std::chrono::milliseconds) {
        shape_ = given;

        return false;
    });

    fit_ = {};
    shaped_ = {};
    upto_.clear();
    total_ = std::chrono::milliseconds{0};
    still_ = 0;
    began_ = std::chrono::steady_clock::now();
}

// Druga droga: obrazek zrobiony w kodzie, dla którego pliku nie ma. Ustawienie jednego źródła
// czyści drugie, żeby nie było stanu, w którym oba coś znaczą.
void Image::hold(Pixels given) {
    raw_.clear();
    whole_ = std::move(given);
    kind_ = Picture::none;
    shape_ = whole_.size();
    frames_ = whole_.stills().size();

    fit_ = {};
    shaped_ = {};
    upto_.clear();
    total_ = std::chrono::milliseconds{0};
    still_ = 0;
    began_ = std::chrono::steady_clock::now();
}

bool Image::held() const { return shape_.wide > 0 && shape_.high > 0; }

int Image::takes() const { return 0; }

// Zatrzymanie zamraża **tę klatkę, którą widać**, a nie zerową. Inaczej pauza przeskakiwałaby obraz.
void Image::play(bool given) {
    if (play_ && !given) still_ = chosen();

    play_ = given;
}

// Ustawienie klatki jest zarazem **przewinięciem**: przesuwamy początek odliczania tak, jakby
// animacja dobiegła właśnie do niej. Dzięki temu jedno wywołanie działa tak samo przy zatrzymanej
// i chodzącej animacji, a próby mogą sprawdzić rachunek czasu bez czekania.
void Image::still(std::size_t given) {
    still_ = given;

    if (upto_.empty()) return;

    still_ = std::min(still_, upto_.size() - 1);

    const std::chrono::milliseconds gone =
        still_ == 0 ? std::chrono::milliseconds{0} : upto_[still_ - 1];

    began_ = std::chrono::steady_clock::now() - gone;
}

std::size_t Image::still() const { return chosen(); }

// Bez wymiaru kratki koło wychodzi jajkiem, więc `fitting` bierze go pod uwagę; zero znaczy
// „nie wiadomo" i wtedy przyjmuje 1:2.
Size Image::wants(Size room) const {
    if (!held()) return {0, 0};

    return fitting(shape_, room, celled(*this));
}

std::optional<std::chrono::milliseconds> Image::due() const {
    if (!play_ || upto_.size() < 2 || total_ <= std::chrono::milliseconds{0}) return {};

    const std::chrono::milliseconds gone =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              began_);

    const std::chrono::milliseconds at{gone.count() % total_.count()};

    const auto found = std::upper_bound(upto_.begin(), upto_.end(), at);

    if (found == upto_.end()) return total_ - at;

    return *found - at;
}

std::size_t Image::chosen() const {
    if (!play_ || upto_.size() < 2 || total_ <= std::chrono::milliseconds{0})
        return upto_.empty() ? still_ : std::min(still_, upto_.size() - 1);

    const std::chrono::milliseconds gone =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              began_);

    const std::chrono::milliseconds at{gone.count() % total_.count()};

    const auto found = std::upper_bound(upto_.begin(), upto_.end(), at);

    if (found == upto_.end()) return upto_.size() - 1;

    return static_cast<std::size_t>(found - upto_.begin());
}

// Przeliczamy **tylko wtedy, gdy zmienił się przydział, siatka albo kratka**. To jest ta różnica,
// której zalążek w demie świadomie nie miał: bez niej zdjęcie z dysku przechodziłoby przez filtr
// dziesięć razy na sekundę.
//
// **I to samo porównanie wystarcza za powiadomienie o zmianie ustawień.** Pierwsza klatka po
// `reconfigure` czyta z korzenia inną gęstość albo inną kratkę, warunek pęka i klatki przeliczają
// się same. Nikt nikomu niczego nie zgłasza.
void Image::spread(Size given) {
    const Blocks kind = grained(*this);
    const Size room = fitting(shape_, given, celled(*this));

    if (shaped_ == given && made_ == kind && placed_ == room && !fit_.stills().empty()) return;

    shaped_ = given;
    made_ = kind;
    fit_ = {};
    placed_ = {};
    upto_.clear();
    total_ = std::chrono::milliseconds{0};

    if (!held() || given.wide <= 0 || given.high <= 0) return;

    placed_ = room;

    const Size step = grain(made_);
    const Size want{placed_.wide * step.wide, placed_.high * step.high};

    std::vector<Still> these;

    if (!raw_.empty()) {
        // Strumień, nie `Pixels`: pełna klatka istnieje przez chwilę i ginie zaraz po zmniejszeniu.
        decoded(raw_, [&](const Bytes& canvas, Size size, std::chrono::milliseconds after) {
            const Pixels one{size, {Still{canvas, after}}};

            these.push_back(one.scaled(want).stills().front());
            these.back().after = after;

            return true;
        });
    } else if (whole_.held()) {
        const Pixels one = whole_.scaled(want);

        these = one.stills();
    }

    if (these.empty()) return;

    fit_ = Pixels{want, std::move(these)};
    frames_ = fit_.stills().size();
    total_ = summed(fit_, upto_);

    still_ = std::min(still_, upto_.size() - 1);
}

// Ustawienie w polu: **na środku**, jak podgląd w Midnight Commanderze. Reszta pola zostaje
// nierysowana, więc widać przez nią tło rodzica — to samo, co robi każdy pojemnik.
View Image::paint(Size given, Area seen) {
    View made{seen.size};

    made.bare({{0, 0}, seen.size});

    spread(given);

    if (fit_.stills().empty()) return made;

    const Style look = skin().get<Style>(role());

    const Spot at{(given.wide - placed_.wide) / 2, (given.high - placed_.high) / 2};

    drawn(made, {at.x - seen.at.x, at.y - seen.at.y}, fit_, chosen(), made_, look);

    return made;
}

}
