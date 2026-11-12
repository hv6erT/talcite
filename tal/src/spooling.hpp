#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"

namespace tal::spooling {

using Offset = std::uint64_t;

class Spool {
public:
    struct Line {
        Offset at = 0;
        Offset upto = 0;
        Offset next = 0;

        friend bool operator==(const Line&, const Line&) = default;
    };

    struct Order {
        Offset from = 0;
        std::size_t most = 0;

        friend bool operator==(const Order&, const Order&) = default;
    };

    explicit Spool(Bytes whole);

    Spool(Offset whole, Bytes head);

    [[nodiscard]] std::optional<Offset> at(std::size_t which);

    [[nodiscard]] std::optional<Line> line(std::size_t which);

    [[nodiscard]] std::size_t found(Offset byte);

    [[nodiscard]] std::optional<std::string_view> read(Offset at, std::size_t much, std::size_t least = SIZE_MAX);

    [[nodiscard]] Offset whole() const { return whole_; }

    [[nodiscard]] std::optional<std::size_t> counted() const { return counted_; }

    void counted(std::size_t lines);

    void counting();

    [[nodiscard]] bool stalled() const { return stalled_; }

    void screen(int high);

    [[nodiscard]] std::optional<Order> ordered();

    void arrived(Offset from, Bytes got);

private:
    struct Anchor {
        std::size_t line = 0;
        Offset at = 0;
    };

    struct Known {
        std::size_t line = 0;
        Line extent;
    };

    struct Walk {
        Anchor from;
        Offset reached = 0;
        bool carriage = false;
    };

    struct Behind {
        Offset from = 0;
        Offset reached = 0;
    };

    enum class Ended { line, short_of, waiting };

    struct Step {
        Ended how = Ended::waiting;
        Line extent;
    };

    [[nodiscard]] bool cached(Offset at, Offset upto) const;

    [[nodiscard]] bool ranged(Offset at, std::size_t much);

    void ask(Offset at, Offset upto);

    void ahead(Offset from);

    [[nodiscard]] char sign(Offset at) const;

    [[nodiscard]] Step forth(Anchor from, Offset limit);

    [[nodiscard]] std::optional<Offset> back(Offset at);

    [[nodiscard]] Anchor start_for(std::size_t which) const;

    [[nodiscard]] Anchor start_at(Offset byte) const;

    void anchored(std::size_t which, Offset at);

    void remember(std::size_t which, Line extent);

    [[nodiscard]] const Known* known(std::size_t which) const;

    [[nodiscard]] const Known* holding(Offset byte) const;

    bool held_ = false;
    Offset whole_ = 0;
    Bytes chunk_;
    Offset chunked_ = 0;
    bool stalled_ = false;
    bool busy_ = false;
    bool counting_ = false;
    std::optional<Order> order_;
    Anchor here_;
    std::optional<Walk> walk_;
    std::optional<Behind> behind_;
    std::vector<Anchor> anchors_;
    std::size_t stride_ = 64;
    std::vector<Known> known_;
    std::optional<std::size_t> counted_;
};

}
