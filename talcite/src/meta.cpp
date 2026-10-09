#include "talcite/meta.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "fscore.hpp"

// **Metadane osadzone w plikach — drzwi rdzenia.** Rozbiór formatów (ID3, komentarze Vorbis, atomy
// MP4) mieszka za szwem, w talcite-adapters, tak jak dekodery obrazów: rdzeń daje typy, interfejs
// dostawcy i ten plik, który otwiera plik, puszcza dostawcę w puli i zamienia jego zdanie odmowy
// w `Refused`. Kontener rozpoznaje dostawca — wołający pyta „co ten plik mówi o sobie" i nie wie,
// czy czyta FLAC, czy MP4.
//
// Dostawca dostaje `meta::Reader`, a nie bryłę bajtów: FLAC waży dziesiątki megabajtów, ID3v1 stoi
// na samym końcu pliku, a atom `moov` w MP4 często też, więc rozbiór skacze po pliku zamiast go
// wczytywać. Odczyt jest synchroniczny, bo cała praca biegnie już w puli, torem `io` — rozbiór
// znaczników to garść porównań bajtów, a czeka się na dysk.
namespace talcite {

namespace {

// Plik otwarty do odczytu, z rozmiarem zapamiętanym przy otwarciu. Odczyt poza końcem oddaje mniej
// bajtów albo pustkę, nigdy błąd — o koniec pliku dostawca pyta przez `size()`, a krótszy odczyt
// to dla niego znak pliku uciętego. Błąd systemu (udział, który zniknął) wychodzi jako `fs::Trouble`
// i odrzuca obietnicę, jak w każdym odczycie `fs`.
class Opened final : public meta::Reader {
public:
    explicit Opened(const fs::path& where)
        : held_(fs::core::open_at(where, fs::Mode::read)), size_(fs::core::sized(*held_)) {}

    [[nodiscard]] std::uintmax_t size() const override { return size_; }

    [[nodiscard]] Bytes read(std::uintmax_t at, std::size_t much) override {
        if (at >= size_ || much == 0) return {};

        const std::uintmax_t left = size_ - at;

        return fs::core::read_at(*held_, at, static_cast<std::size_t>(std::min<std::uintmax_t>(much, left)));
    }

private:
    std::shared_ptr<fs::File::Held> held_;
    std::uintmax_t size_;
};

}

MetaProvider& Seam<MetaProvider>::provider() const {
    if (provider_ == nullptr) throw Unbound{MetaProvider::kind()};

    return *provider_;
}

// Szew bez dostawcy odrzuca obietnicę (`Unbound`), a nie rzuca od razu: wołający obietnicy ma
// jedną drogę błędu, a nie dwie.
Promise<meta::Description> Seam<MetaProvider>::described(fs::path where) const {
    return Promise<meta::Description>{[held = provider_, where = std::move(where)](auto resolve, auto) {
        if (held == nullptr) throw Unbound{MetaProvider::kind()};

        Opened from{where};
        meta::Description made;

        if (const std::string said = held->describe(from, made); !said.empty())
            throw Refused{MetaProvider::kind(), said};

        resolve(std::move(made));
    }};
}

Promise<Bytes> Seam<MetaProvider>::pictured(fs::path where, std::size_t which) const {
    return Promise<Bytes>{[held = provider_, where = std::move(where), which](auto resolve, auto) {
        if (held == nullptr) throw Unbound{MetaProvider::kind()};

        Opened from{where};
        Bytes made;

        if (const std::string said = held->picture(from, which, made); !said.empty())
            throw Refused{MetaProvider::kind(), said};

        resolve(std::move(made));
    }};
}

}
