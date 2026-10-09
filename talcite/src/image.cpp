#include "talcite/image.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "talcite/root.hpp"
#include "talcite/sixel.hpp"
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

// Barwność i znaki też są faktem o powierzchni: czarno-białe rysuje samą sylwetkę, a bez Unicode
// nie ma znaków blokowych. Poza drzewem — pełne kolory i Unicode, jak przy milczącym terminalu.
[[nodiscard]] Colors colored(const Node& one) {
    const Root* top = one.root();

    return top != nullptr ? top->colors() : Colors::rgb;
}

[[nodiscard]] bool lettered(const Node& one) {
    const Root* top = one.root();

    return top != nullptr ? top->unicode() : true;
}

[[nodiscard]] Size celled(const Node& one) {
    const Root* top = one.root();

    return top != nullptr ? top->cell() : Size{};
}

// **Piksele, gdy powierzchnia je przyjmie** (G1, G2): terminal mówi sixel albo kitty, a kratka ma
// znany wymiar — bez niego nie wiadomo, ile pikseli zamówić. `none` znaczy „znaki".
[[nodiscard]] Graphics sharp(const Node& one) {
    const Root* top = one.root();

    if (top == nullptr || top->cell().wide <= 0 || top->cell().high <= 0) return Graphics::none;

    return top->graphics();
}

// Znak obrazu: niepowtarzalny w całym programie, bo kratki różnych obrazów spotykają się w jednej
// klatce. Zero znaczy „żaden obraz".
[[nodiscard]] std::uint32_t marked() {
    static std::atomic<std::uint32_t> next{0};

    return ++next;
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

// **Trzymamy źródło, nie obrazek.** To jest lekcja z `frk::Animation` posunięta do końca: tam klatki
// leżały skompresowane i rozwijała się jedna naraz, tu w pamięci stoją klatki **już w rozdzielczości
// kratek**, a pełnej rozdzielczości nie ma nigdy — źródło (`Figure::fetch`, z `figured` nad bajtami
// i szwem, albo cokolwiek, co umie oddać piksele w zadanym rozmiarze) dekoduje na puli i oddaje
// klatki już zmniejszone. Widżet nie zna ani bajtów, ani szwu: rodzaj i kształt dostaje gotowe,
// od ręki — układ pyta o nie przed malowaniem — a piksele zamawia, gdy w `paint` pozna rozmiar.
// Źródło bez kształtu (bez dostawcy) zostawia widżet pusty od razu; odmowa źródła opróżnia go,
// gdy wróci zamówienie. Liczbę klatek widżet zna, gdy wróci pierwsze.
void Image::hold(Figure given) {
    fetch_ = std::move(given.fetch);
    whole_ = {};
    format_ = given.format;
    size_ = fetch_ ? given.size : Size{};
    frames_ = 0;

    ordered_.reset();
    ordering_ = {};
    fit_ = {};
    shaped_ = {};
    placed_ = {};
    fine_ = Graphics::none;
    sharpened();
    upto_.clear();
    total_ = std::chrono::milliseconds{0};
    still_ = 0;
    began_ = std::chrono::steady_clock::now();
}

// Druga droga: obrazek zrobiony w kodzie, dla którego pliku nie ma. Ustawienie jednego źródła
// czyści drugie, żeby nie było stanu, w którym oba coś znaczą.
void Image::hold(Pixels given) {
    fetch_ = {};
    ordered_.reset();
    ordering_ = {};
    whole_ = std::move(given);
    format_ = Format::none;
    size_ = whole_.size();
    frames_ = whole_.stills().size();

    fit_ = {};
    shaped_ = {};
    fine_ = Graphics::none;
    sharpened();
    upto_.clear();
    total_ = std::chrono::milliseconds{0};
    still_ = 0;
    began_ = std::chrono::steady_clock::now();
}

// Zwolnienie: bez źródła, bez klatek, bez rodzaju — to, co `hold(Pixels{})` robiło mimochodem,
// nazwane po imieniu. Kartka zamykana ma nie trzymać ani pliku w domknięciu źródła, ani klatek.
void Image::clear() { hold(Pixels{}); }

bool Image::held() const { return size_.wide > 0 && size_.high > 0; }

// Zajęty, póki czeka na cokolwiek z puli: na klatki albo — przy pikselach — na ich sixel.
bool Image::busy() const { return ordered_.has_value() || encoded_.has_value(); }

int Image::capacity() const { return 0; }

// Zatrzymanie zamraża **tę klatkę, którą widać**, a nie zerową. Inaczej pauza przeskakiwałaby obraz.
// Wznowienie rusza od zamrożonej: przewija zegar tak, jakby animacja właśnie do niej doszła — bez
// tego po pauzie obraz przeskakiwał o tyle klatek, ile minęło czasu od pokazania.
void Image::play(bool given) {
    if (play_ == given) return;

    if (!given) still_ = chosen();

    play_ = given;

    if (given) still(still_);
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

    return fitting(size_, room, celled(*this));
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
//
// **Plik nie przelicza się tu, tylko zamawia** — kształt szpuli: w `paint` widżet wie, ile ma
// miejsca, więc tu liczy rozmiar docelowy i woła z nim źródło; wynik wraca
// przez pętlę i wchodzi, jeśli rozmiar wciąż ten sam — inaczej następny `paint` zamawia od nowa.
// Do tego czasu widżet maluje to, co miał (przy zmianie rozmiaru stare klatki, bez migotania),
// albo nic. Jedno zamówienie naraz; nowe zastępuje stare, a stary wynik poznaje się po rozmiarze.
// Wskaźnik do siebie słaby: wynik dla widżetu, który zginął, nie ma dokąd wejść. Obrazek z kodu
// (`hold(Pixels)`) przelicza się od ręki, bo jest w pamięci i jest mały.
void Image::spread(Size given) {
    const Blocks kind = grained(*this);
    const bool unicode = lettered(*this);
    const Graphics fine = sharp(*this);
    const Size room = fitting(size_, given, celled(*this));

    if (shaped_ == given && made_ == kind && unicode_ == unicode && placed_ == room && fine_ == fine &&
        !fit_.stills().empty())
        return;

    if (!held() || given.wide <= 0 || given.high <= 0) {
        shaped_ = given;
        made_ = kind;
        unicode_ = unicode;
        fit_ = {};
        placed_ = {};
        fine_ = fine;
        sharpened();
        upto_.clear();
        total_ = std::chrono::milliseconds{0};
        ordered_.reset();

        return;
    }

    // Przy pikselach zamawiamy pełną rozdzielczość pola — kratki razy piksele kratki — a znaki,
    // na czas, gdy obrazu nie widać w całości, liczymy z niej w `sharpened`.
    const Size step = fine != Graphics::none ? celled(*this) : grain(kind, unicode);
    const Size want{room.wide * step.wide, room.high * step.high};

    if (fetch_) {
        if (ordered_.has_value() && ordering_ == want) return;

        shaped_ = given;
        made_ = kind;
        unicode_ = unicode;
        ordering_ = want;

        const std::weak_ptr<Image> weak = std::static_pointer_cast<Image>(weak_from_this().lock());

        // Odmowa źródła (odrzucona obietnica) i pusty wynik kończą się tak samo: widżet pusty,
        // rodzaj zostaje — widżet ma pokazać, co może; kto chce zdania odmowy, dekoduje szwem sam.
        const auto emptied = [](Image& self) {
            self.size_ = {};
            self.fit_ = {};
            self.placed_ = {};
            self.frames_ = 0;
            self.sharpened();
        };

        ordered_.emplace(fetch_(want)
                             .here()
                             .then([weak, want, room, fine, emptied](Pixels made) {
                                 const auto self = weak.lock();

                                 if (!self || self->ordering_ != want) return;

                                 self->ordered_.reset();

                                 if (!made.held()) {
                                     emptied(*self);

                                     return;
                                 }

                                 self->fit_ = std::move(made);
                                 self->placed_ = room;
                                 self->fine_ = fine;
                                 self->frames_ = self->fit_.stills().size();
                                 self->total_ = summed(self->fit_, self->upto_);
                                 self->still_ = std::min(self->still_, self->upto_.size() - 1);
                                 self->sharpened();
                             })
                             .fail([weak, want, emptied](const std::exception&) {
                                 const auto self = weak.lock();

                                 if (!self || self->ordering_ != want) return;

                                 self->ordered_.reset();
                                 emptied(*self);
                             }));

        ordered_->start();

        return;
    }

    shaped_ = given;
    made_ = kind;
    unicode_ = unicode;
    fit_ = {};
    placed_ = {};
    fine_ = fine;
    upto_.clear();
    total_ = std::chrono::milliseconds{0};

    const Pixels one = whole_.held() ? whole_.scaled(want) : Pixels{};

    if (!one.stills().empty()) {
        placed_ = room;
        fit_ = one;
        frames_ = fit_.stills().size();
        total_ = summed(fit_, upto_);

        still_ = std::min(still_, upto_.size() - 1);
    }

    sharpened();
}

// **Klatki w pikselach i ich zastępstwo w znakach.** Przy pikselach `fit_` ma pełną rozdzielczość
// pola, więc znaki — rysowane, gdy obrazu nie widać w całości, i zanim sixel będzie gotów — liczą się
// z niej tu, raz. Sixel koduje się na puli, torem `cpu`, wszystkie klatki naraz; do tego czasu obraz
// stoi w znakach. Wyniku dla klatek, których już nie ma, nie trzeba rozpoznawać: porzucona obietnica
// (`encoded_.reset()`) jest odwołana i jej `then` nie przychodzi.
//
// **Kitty nie koduje niczego tutaj** (G2): terminal dostaje surowe piksele, a wysyła je `Term`, raz
// na klatkę obrazu. Obrazy są więc gotowe od razu — wszystkie klatki dzielą jedne piksele, a jak
// długo ktoś je trzyma, tak długo terminal może je trzymać u siebie.
void Image::sharpened() {
    encoded_.reset();
    pictures_.clear();
    coarse_ = {};

    if (fine_ == Graphics::none || fit_.stills().empty()) return;

    const Size step = grain(made_, unicode_);

    coarse_ = fit_.scaled({placed_.wide * step.wide, placed_.high * step.high});

    if (fine_ == Graphics::kitty) {
        const auto shared = std::make_shared<const Pixels>(fit_);

        for (std::size_t at = 0; at < fit_.stills().size(); ++at)
            pictures_.push_back(Picture{marked(), placed_, nullptr, shared, at});

        return;
    }

    const std::weak_ptr<Image> weak = std::static_pointer_cast<Image>(weak_from_this().lock());
    const Size room = placed_;

    using Sixels = std::vector<std::shared_ptr<const std::string>>;

    encoded_.emplace(Promise<Sixels>{[source = fit_](auto resolve, auto) {
                                         Sixels made;

                                         made.reserve(source.stills().size());

                                         for (std::size_t at = 0; at < source.stills().size(); ++at)
                                             made.push_back(std::make_shared<const std::string>(sixel(source, at)));

                                         resolve(std::move(made));
                                     },
                                     Lane::cpu}
                         .here()
                         .then([weak, room](Sixels made) {
                             const auto self = weak.lock();

                             if (!self) return;

                             self->encoded_.reset();

                             for (std::shared_ptr<const std::string>& one : made)
                                 self->pictures_.push_back(Picture{marked(), room, std::move(one)});
                         }));

    encoded_->start();
}

// Ustawienie w polu: **na środku**, jak podgląd w Midnight Commanderze. Reszta pola zostaje
// nierysowana, więc widać przez nią tło rodzica — to samo, co robi każdy pojemnik.
View Image::paint(Size given, Area seen) {
    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    spread(given);

    if (fit_.stills().empty()) return made;

    const Style look = skin().style(role());

    const Spot at{(given.wide - placed_.wide) / 2, (given.high - placed_.high) / 2};

    const Spot from{at.x - seen.at.x, at.y - seen.at.y};
    const std::size_t which = chosen();

    drawn(made, from, fine_ != Graphics::none ? coarse_ : fit_, which, made_, look, colored(*this), unicode_);

    if (fine_ != Graphics::none && which < pictures_.size()) made.picture({from, placed_}, pictures_[which]);

    return made;
}

}
