#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "talcite/bytes.hpp"
#include "talcite/clipboard.hpp"
#include "talcite/comb.hpp"
#include "talcite/feed.hpp"
#include "talcite/page.hpp"
#include "talcite/promise.hpp"

namespace talcite::ui {

class Editor : public Page {
public:
    class Selection {
    public:
        Selection(const Selection&) = delete;
        Selection& operator=(const Selection&) = delete;
        Selection(Selection&&) = delete;
        Selection& operator=(Selection&&) = delete;

        [[nodiscard]] std::size_t anchor() const;

        [[nodiscard]] std::size_t from() const;

        [[nodiscard]] std::size_t upto() const;

        [[nodiscard]] bool empty() const;

        [[nodiscard]] std::string text() const;

        void set(std::size_t anchor, std::size_t cursor, Moved how = Moved::by_program);

        void extend(std::size_t to, Moved how = Moved::by_program);

        void clear();

        bool erase(Moved how = Moved::by_program);

    private:
        friend class Editor;

        explicit Selection(Editor& owner);
        ~Selection() = default;

        Editor& owner_;
    };

    class Block {
    public:
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        Block(Block&&) = delete;
        Block& operator=(Block&&) = delete;

        [[nodiscard]] bool empty() const;

        [[nodiscard]] bool pending() const;

        [[nodiscard]] std::size_t from() const;

        [[nodiscard]] std::size_t upto() const;

        [[nodiscard]] std::string text() const;

        void mark();

        void clear();

        bool copy();

        bool move();

        bool erase();

    private:
        friend class Editor;

        explicit Block(Editor& owner);
        ~Block() = default;

        Editor& owner_;
    };

    Editor();

    explicit Editor(Seam<Clipboard> with);

    explicit Editor(ScrollShape shape, Seam<Clipboard> with = clipboard());

    ~Editor() override;

    void hold(Bytes given);

    [[nodiscard]] bool dirty() const;

    void clean();

    [[nodiscard]] Promise<void> save(Drain into);

    [[nodiscard]] bool busy() const;

    [[nodiscard]] bool undoable() const;

    [[nodiscard]] bool redoable() const;

    void undo();

    void redo();

    [[nodiscard]] bool readonly() const;

    void readonly(bool given);

    [[nodiscard]] std::size_t cursor() const;

    void cursor(std::size_t given, Moved how = Moved::by_program);

    [[nodiscard]] std::size_t line() const;

    void line(std::size_t which);

    [[nodiscard]] std::size_t column();

    [[nodiscard]] Selection& selection() { return selection_; }

    [[nodiscard]] const Selection& selection() const { return selection_; }

    [[nodiscard]] Block& block() { return block_; }

    [[nodiscard]] const Block& block() const { return block_; }

    [[nodiscard]] std::string text() const;

    bool copy();

    bool cut();

    bool paste();

    void insert(std::string what, Moved how = Moved::by_program);

    void back_erase(std::size_t much, Moved how = Moved::by_program);

    void front_erase(std::size_t much, Moved how = Moved::by_program);

    std::optional<Hit> find(const Comb& how, std::size_t from, bool back);

    bool replace(const Comb& how, const std::string& with);

    std::size_t replace_all(const Comb& how, const std::string& with);

    [[nodiscard]] std::optional<Spot> caret() const override;

protected:
    View paint(Size given, Area seen) override;

    using Page::handle;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;

    bool handle(DoubleClickEvent& what) override;

    bool handle(MouseEvent& what) override;

    bool handle(PasteEvent& what) override;

private:
    void settled(std::size_t where, Moved how = Moved::by_program);

    struct Kept;

    std::unique_ptr<Kept> kept_;
    Selection selection_;
    Block block_;
};

}
