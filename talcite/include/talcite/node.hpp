#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "talcite/area.hpp"
#include "talcite/handler.hpp"
#include "talcite/shape.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

class Node;
class Root;
class Skin;

class Children {
public:
    Children(const Children&) = delete;
    Children& operator=(const Children&) = delete;
    Children(Children&&) = delete;
    Children& operator=(Children&&) = delete;

    void add(std::shared_ptr<Node> given);

    void add(std::vector<std::shared_ptr<Node>> given);

    std::shared_ptr<Node> remove(Node* one);

    void clear();

    [[nodiscard]] std::size_t many() const { return inside_.size(); }

    [[nodiscard]] bool empty() const { return inside_.empty(); }

    [[nodiscard]] std::size_t changes() const { return changes_; }

    [[nodiscard]] bool contains(const Node* one) const;

    [[nodiscard]] std::optional<std::size_t> find(const Node& one) const;

    [[nodiscard]] const std::shared_ptr<Node>& operator[](std::size_t which) const {
        if (which >= inside_.size())
            throw std::out_of_range("talcite::ui::Children: no such child");

        return inside_[which];
    }

    [[nodiscard]] const std::shared_ptr<Node>& front() const {
        if (inside_.empty()) throw std::out_of_range("talcite::ui::Children: no children");

        return inside_.front();
    }

    [[nodiscard]] const std::shared_ptr<Node>& back() const {
        if (inside_.empty()) throw std::out_of_range("talcite::ui::Children: no children");

        return inside_.back();
    }

    using walk = std::vector<std::shared_ptr<Node>>::const_iterator;

    [[nodiscard]] walk begin() const { return inside_.begin(); }

    [[nodiscard]] walk end() const { return inside_.end(); }

private:
    friend class Node;

    explicit Children(Node& whose) : whose_(&whose) {}

    ~Children();

    Node* whose_;
    std::vector<std::shared_ptr<Node>> inside_;
    std::size_t changes_ = 0;
};

class Node : public Handler, public std::enable_shared_from_this<Node> {
public:
    Node();

    ~Node() override;

    [[nodiscard]] Children& children() { return inside_; }

    [[nodiscard]] const Children& children() const { return inside_; }

    [[nodiscard]] Node* parent() const override { return up_; }

    [[nodiscard]] Root* root() const;

    [[nodiscard]] const Area& area() const { return area_; }

    [[nodiscard]] std::optional<Area> screen_area() const;

    [[nodiscard]] std::optional<Area> area_in(const Node& top) const;

    [[nodiscard]] Spot local(Spot where) const;

    [[nodiscard]] const std::string& role() const { return role_; }

    virtual void role(std::string given);

    [[nodiscard]] virtual int capacity() const;

    [[nodiscard]] virtual bool accepts(const Node& one) const;

    void unfocus();

    [[nodiscard]] virtual bool focusable() const;

    [[nodiscard]] bool focused() const;

    virtual void focus();

    View render(const Area& where);

    View render(const Area& where, const Area& seen);

    [[nodiscard]] virtual Size wants(Size room) const;

    [[nodiscard]] virtual Area placement(Size room) const;

    [[nodiscard]] virtual std::optional<Spot> caret() const;

    [[nodiscard]] virtual CaretShape caret_shape() const;

    void caret_shape(CaretShape given);

protected:
    bool broadcast(KeyEvent& what) {
        const std::vector<std::shared_ptr<Node>> these{inside_.begin(), inside_.end()};

        for (const std::shared_ptr<Node>& one : these) {
            if (offer(what, *one)) return true;

            if (one->focusable()) continue;

            if (one->broadcast(what)) return true;
        }

        return false;
    }

    bool offer(KeyEvent& what, Node& one) {
        return one.trigger(ShortcutEvent{what.key()});
    }

    [[nodiscard]] const Skin& skin() const;

    virtual View paint(Size given, Area seen);

private:
    friend class Children;
    friend class Root;

    Node* up_ = nullptr;
    std::string role_;
    Children inside_;
    Area area_;
    Area seen_;
    CaretShape caret_shape_;
};

}
