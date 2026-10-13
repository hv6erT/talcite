#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/feed.hpp"
#include "talcite/promise.hpp"

namespace talcite {

class Spool : public std::enable_shared_from_this<Spool> {
public:
    using Peek = std::function<Bytes(std::size_t at, std::size_t most)>;

    struct Line {
        std::size_t at = 0;
        std::size_t upto = 0;
        std::size_t next = 0;
    };

    Spool(Feed feed, std::size_t whole, Bytes head = {});

    Spool(Peek peek, std::size_t whole);

    explicit Spool(Bytes given);

    [[nodiscard]] std::optional<std::size_t> at(std::size_t which);

    [[nodiscard]] std::optional<Line> line(std::size_t which);

    [[nodiscard]] std::size_t found(std::size_t byte);

    [[nodiscard]] std::optional<std::string_view> read(std::size_t at, std::size_t much, std::size_t least = SIZE_MAX);

    [[nodiscard]] std::size_t whole() const { return whole_; }

    [[nodiscard]] std::optional<std::size_t> counted() const { return counted_; }

    void counted(std::size_t lines);

    [[nodiscard]] bool stalled() const { return stalled_; }

    void counting();

    void awoken(std::function<void()> given);

    void screen(int high);

    void changed(std::size_t at, std::size_t gone, std::size_t came, std::ptrdiff_t lines);

    void rewritten(std::size_t at, std::ptrdiff_t bytes, std::ptrdiff_t lines);

private:
    struct Anchor {
        std::size_t line = 0;
        std::size_t at = 0;
    };

    struct Known {
        std::size_t line = 0;
        Line extent;
        bool open = false;
    };

    struct Walk {
        Anchor from;
        std::size_t reached = 0;
        bool carriage = false;
    };

    struct Behind {
        std::size_t from = 0;
        std::size_t reached = 0;
    };

    struct Want {
        std::size_t at = 0;
        std::size_t upto = 0;
    };

    enum class Ended : std::uint8_t { line, short_of, waiting };

    struct Step {
        Ended how = Ended::waiting;
        Line extent;
    };

    [[nodiscard]] bool cached(std::size_t at, std::size_t upto) const;

    [[nodiscard]] bool ranged(std::size_t at, std::size_t much);

    void ask(std::size_t at, std::size_t upto);

    void arrived(std::size_t from, Bytes got);

    void ahead(std::size_t from);

    [[nodiscard]] char sign(std::size_t at) const;

    [[nodiscard]] Step forth(Anchor from, std::size_t limit);

    [[nodiscard]] std::optional<std::size_t> back(std::size_t at);

    [[nodiscard]] Anchor nearest(std::size_t which) const;

    [[nodiscard]] Anchor start_for(std::size_t which) const;

    [[nodiscard]] Anchor start_at(std::size_t byte) const;

    void anchored(std::size_t which, std::size_t at);

    void remember(std::size_t which, Line extent, bool open);

    [[nodiscard]] const Known* known(std::size_t which) const;

    [[nodiscard]] const Known* holding(std::size_t byte) const;

    [[nodiscard]] const Known* ending(std::size_t byte) const;

    Feed feed_;
    Peek peek_;
    Bytes held_;
    std::size_t whole_ = 0;

    Bytes chunk_;
    std::size_t chunked_ = 0;

    bool stalled_ = false;
    bool busy_ = false;
    bool counting_ = false;
    std::optional<Want> wanted_;
    std::optional<Promise<void>> ordered_;
    std::function<void()> awoken_;

    Anchor here_;
    std::optional<Walk> walk_;
    std::optional<Behind> behind_;

    std::vector<Anchor> anchors_;
    std::size_t stride_ = 64;

    std::vector<Known> known_;

    std::optional<std::size_t> counted_;
};

}
