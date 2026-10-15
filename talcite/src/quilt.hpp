#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/bytes.hpp"

namespace talcite {

class Quilt {
public:
    explicit Quilt(Bytes given);

    [[nodiscard]] std::size_t whole() const { return whole_; }

    [[nodiscard]] Bytes bytes(std::size_t at, std::size_t most);

    struct Piece {
        bool added = false;
        std::size_t at = 0;
        std::size_t much = 0;
    };

    struct Scrap {
        std::vector<Piece> pieces;
        std::size_t much = 0;
    };

    void insert(std::size_t at, std::string_view what);

    void erase(std::size_t at, std::size_t much);

    [[nodiscard]] Scrap taken(std::size_t at, std::size_t much);

    [[nodiscard]] Scrap stored(std::string_view what);

    void put(std::size_t at, const Scrap& what);

    [[nodiscard]] std::size_t lines(const Scrap& what) const;

    [[nodiscard]] std::string text(const Scrap& what) const;

    [[nodiscard]] char last(const Scrap& what) const;

    static void joined(Scrap& into, const Scrap& more, bool front);

    [[nodiscard]] std::size_t appended() const { return added_.size(); }

    struct Cut {
        std::size_t at = 0;
        std::size_t much = 0;
        std::string_view text;
    };

    void spliced(const std::vector<Cut>& cuts);

private:
    struct Patch {
        std::vector<Piece> pieces;
        std::size_t much = 0;
    };

    struct Spot {
        std::size_t patch = 0;
        std::size_t piece = 0;
        std::size_t gone = 0;
    };

    [[nodiscard]] Spot located(std::size_t at);

    [[nodiscard]] Spot split(std::size_t at);

    bool sewn(Spot left);

    void mended(std::size_t patch);

    Bytes held_;

    std::string added_;

    std::vector<Patch> patches_;
    std::size_t whole_ = 0;

    std::vector<std::size_t> starts_;
    bool stale_ = true;
};

}
