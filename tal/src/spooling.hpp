#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/editing.hpp"

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

    Spool() = default;

    virtual ~Spool();

    Spool(const Spool&) = delete;
    Spool& operator=(const Spool&) = delete;

    [[nodiscard]] virtual std::optional<Offset> at(std::size_t which) = 0;

    [[nodiscard]] virtual std::optional<Line> line(std::size_t which) = 0;

    [[nodiscard]] virtual std::size_t found(Offset byte) = 0;

    [[nodiscard]] std::optional<std::string_view> read(Offset at, std::size_t much) { return read(at, much, SIZE_MAX); }

    [[nodiscard]] virtual std::optional<std::string_view> read(Offset at, std::size_t much, std::size_t least) = 0;

    [[nodiscard]] virtual Offset whole() const = 0;

    [[nodiscard]] virtual std::optional<std::size_t> counted() const = 0;

    virtual void counted(std::size_t lines) = 0;

    virtual void counting() = 0;

    [[nodiscard]] virtual bool stalled() const = 0;

    virtual void screen(int high) = 0;

    [[nodiscard]] virtual std::optional<Order> ordered() = 0;

    virtual void arrived(Offset from, Bytes got) = 0;
};

class Reel : public Spool {
public:
    explicit Reel(Bytes whole);

    Reel(Offset whole, Bytes head);

    using Spool::read;

    [[nodiscard]] std::optional<Offset> at(std::size_t which) override;

    [[nodiscard]] std::optional<Line> line(std::size_t which) override;

    [[nodiscard]] std::size_t found(Offset byte) override;

    [[nodiscard]] std::optional<std::string_view> read(Offset at, std::size_t much, std::size_t least) override;

    [[nodiscard]] Offset whole() const override { return whole_; }

    [[nodiscard]] std::optional<std::size_t> counted() const override { return counted_; }

    void counted(std::size_t lines) override;

    void counting() override;

    [[nodiscard]] bool stalled() const override { return stalled_; }

    void screen(int high) override;

    [[nodiscard]] std::optional<Order> ordered() override;

    void arrived(Offset from, Bytes got) override;

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

class Lines : public Spool {
public:
    explicit Lines(std::shared_ptr<const Text> text);

    using Spool::read;

    [[nodiscard]] std::optional<Offset> at(std::size_t which) override;

    [[nodiscard]] std::optional<Line> line(std::size_t which) override;

    [[nodiscard]] std::size_t found(Offset byte) override;

    [[nodiscard]] std::optional<std::string_view> read(Offset at, std::size_t much, std::size_t least) override;

    [[nodiscard]] Offset whole() const override { return whole_; }

    [[nodiscard]] std::optional<std::size_t> counted() const override { return text_->lines(); }

    void counted(std::size_t lines) override;

    void counting() override;

    [[nodiscard]] bool stalled() const override { return false; }

    void screen(int high) override;

    [[nodiscard]] std::optional<Order> ordered() override;

    void arrived(Offset from, Bytes got) override;

    void edited(std::size_t from, std::size_t upto, std::size_t end);

private:
    void built();

    [[nodiscard]] Offset before(std::size_t which) const;

    void grown(std::size_t which, std::int64_t by);

    std::shared_ptr<const Text> text_;
    std::vector<Offset> tree_;
    Offset whole_ = 0;
    std::string piece_;
};

}
