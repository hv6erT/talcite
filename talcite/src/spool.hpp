#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/feed.hpp"

namespace talcite {

// **Szpula: źródło bajtowe podane jako wiersze.** Przewija się w przód i w tył po dokumencie,
// którego nie da się mieć całego naraz, i nigdy go nie indeksuje — pamięta tylko okno bajtów,
// miejsce, w którym stoi, i garść kotwic.
//
// Nie zna szerokości okna, skórki ani drzewa. Nie rozwija tabulatorów i nie zawija: oddaje surowy
// wiersz bez `\n` i bez `\r` na końcu. Wszystko, co dotyczy rysowania, zostaje w `Viewer`.
class Spool {
public:
    Spool(Feed feed, std::size_t whole);

    explicit Spool(Bytes given);

    [[nodiscard]] std::vector<std::string> lines(std::size_t which, std::size_t most);

    [[nodiscard]] std::optional<std::size_t> at(std::size_t which);

    [[nodiscard]] std::size_t found(std::size_t at);

    [[nodiscard]] std::size_t whole() const { return whole_; }

    [[nodiscard]] std::optional<std::size_t> counted() const { return counted_; }

    void screen(int high);

    void changed(std::size_t at, std::ptrdiff_t bytes, std::ptrdiff_t lines);

private:
    struct Anchor {
        std::size_t line = 0;
        std::size_t at = 0;
    };

    [[nodiscard]] bool ranged(std::size_t at, std::size_t much);

    [[nodiscard]] char sign(std::size_t at) const;

    [[nodiscard]] std::size_t forth(std::size_t at, std::string* into);

    [[nodiscard]] std::size_t back(std::size_t at);

    [[nodiscard]] Anchor nearest(std::size_t which) const;

    void anchored(std::size_t which, std::size_t at);

    Feed feed_;
    Bytes held_;
    std::size_t whole_ = 0;

    Bytes chunk_;
    std::size_t chunked_ = 0;

    Anchor here_;

    std::vector<Anchor> anchors_;
    std::size_t stride_ = 64;

    std::optional<std::size_t> counted_;
};

}
