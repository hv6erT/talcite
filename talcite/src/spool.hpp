#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/feed.hpp"
#include "talcite/promise.hpp"

namespace talcite {

class Spool : public std::enable_shared_from_this<Spool> {
public:
    using Peek = std::function<Bytes(std::size_t at, std::size_t most)>;

    Spool(Feed feed, std::size_t whole, Bytes head = {});

    Spool(Peek peek, std::size_t whole);

    explicit Spool(Bytes given);

    [[nodiscard]] std::optional<std::vector<std::string>> lines(std::size_t which, std::size_t most);

    [[nodiscard]] std::optional<std::size_t> at(std::size_t which);

    [[nodiscard]] std::size_t found(std::size_t at);

    [[nodiscard]] std::size_t whole() const { return whole_; }

    [[nodiscard]] std::optional<std::size_t> counted() const { return counted_; }

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

    struct Want {
        std::size_t at = 0;
        std::size_t upto = 0;
    };

    [[nodiscard]] bool cached(std::size_t at, std::size_t upto) const;

    [[nodiscard]] bool ranged(std::size_t at, std::size_t much);

    void ask(std::size_t at, std::size_t upto);

    void arrived(std::size_t from, Bytes got);

    [[nodiscard]] char sign(std::size_t at) const;

    [[nodiscard]] std::size_t forth(std::size_t at, std::string* into);

    [[nodiscard]] std::size_t back(std::size_t at);

    [[nodiscard]] Anchor nearest(std::size_t which) const;

    void anchored(std::size_t which, std::size_t at);

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

    std::vector<Anchor> anchors_;
    std::size_t stride_ = 64;

    std::optional<std::size_t> counted_;
};

}
