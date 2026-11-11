#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"

namespace tal {

inline constexpr int endless = std::numeric_limits<int>::max();

enum class Effect { none, paint, layout };

struct Share {
    int weight = 0;
    int least = 0;

    friend bool operator==(const Share&, const Share&) = default;
};

struct Caret {
    enum class Form { usual, block, underline, bar };

    Spot at;
    Form form = Form::usual;
    bool blink = true;

    friend bool operator==(const Caret&, const Caret&) = default;
};

class Children {
public:
    class Iterator {
    public:
        using Held = std::vector<std::shared_ptr<Node>>::const_iterator;

        explicit Iterator(Held at) : at_(at) {}

        [[nodiscard]] Node& operator*() const { return **at_; }

        Iterator& operator++() {
            ++at_;

            return *this;
        }

        friend bool operator==(const Iterator&, const Iterator&) = default;

    private:
        Held at_;
    };

    Children(const Children&) = delete;
    Children& operator=(const Children&) = delete;

    template <typename T, typename... Given>
    T& add(Given&&... given);

    Node& add(std::shared_ptr<Node> one);

    Node& insert(std::size_t at, std::shared_ptr<Node> one);

    std::shared_ptr<Node> remove(Node& one);

    void clear();

    [[nodiscard]] std::size_t size() const { return held_.size(); }

    [[nodiscard]] bool empty() const { return held_.empty(); }

    [[nodiscard]] Iterator begin() const { return Iterator{held_.begin()}; }

    [[nodiscard]] Iterator end() const { return Iterator{held_.end()}; }

private:
    friend class Node;
    friend class Document;

    explicit Children(Node& owner) : owner_(owner) {}

    Node& owner_;
    std::vector<std::shared_ptr<Node>> held_;
};

class Node : public std::enable_shared_from_this<Node> {
public:
    Node();

    virtual ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    void id(std::string name);

    [[nodiscard]] const std::string& id() const { return id_; }

    void classes(std::vector<std::string> names);

    void add_class(std::string_view one);

    void drop_class(std::string_view one);

    [[nodiscard]] bool has_class(std::string_view one) const;

    [[nodiscard]] States states() const { return states_; }

    [[nodiscard]] Node* parent() const { return parent_; }

    [[nodiscard]] Document* document() const { return document_; }

    [[nodiscard]] Children& children() { return children_; }

    [[nodiscard]] const Children& children() const { return children_; }

    template <typename T>
    [[nodiscard]] T* find(std::string_view id) {
        return dynamic_cast<T*>(found(id));
    }

    [[nodiscard]] Size measured(Limits room);

    void placed(Area outer);

    [[nodiscard]] Area area() const { return area_; }

    void share(Share how);

    [[nodiscard]] Share share() const { return share_; }

    template <typename E>
    [[nodiscard]] Hook on(Fn<E> what, Phase phase = Phase::bubble) {
        auto made = std::make_shared<detail::Typed<E>>(std::move(what));

        made->phase = phase;

        return hooked(typeid(E), std::move(made));
    }

    void enable(bool on);

    [[nodiscard]] bool enabled() const { return enabled_; }

    [[nodiscard]] bool focusable() const;

    void focus();

    [[nodiscard]] bool focused() const;

protected:
    template <typename E>
    [[nodiscard]] Hook otherwise(Fn<E> what) {
        auto made = std::make_shared<detail::Typed<E>>(std::move(what));

        made->fallback = true;

        return hooked(typeid(E), std::move(made));
    }

    void focusable(bool on);

    void capture();

    void state(State one, bool on);

    [[nodiscard]] const Style& style() const { return style_; }

    [[nodiscard]] Look look(std::string_view part = {}) const;

    [[nodiscard]] std::string_view glyph(std::string_view name) const;

    [[nodiscard]] text::Rendering rendering() const;

    [[nodiscard]] virtual Size measure(Limits room) const;

    virtual void arrange(Area inside);

    virtual void paint(Canvas& onto) const;

    virtual void outline(Canvas& box) const;

    [[nodiscard]] virtual std::optional<Caret> caret() const;

    void relayout();

    void repaint();

    void notified(Event& one);

private:
    friend class Children;
    friend class Document;

    template <typename T>
    friend class Property;

    [[nodiscard]] Node* found(std::string_view id);

    [[nodiscard]] Hook hooked(std::type_index type, std::shared_ptr<detail::Slot> slot);

    [[nodiscard]] std::vector<std::shared_ptr<detail::Slot>> slots(const Event& one, bool fallback, Phase phase) const;

    bool called(Event& one, bool fallback, Phase phase);

    void changed(const void* source);

    [[nodiscard]] bool inert() const;

    [[nodiscard]] bool holds(const Node& one) const;

    [[nodiscard]] Sides box() const;

    [[nodiscard]] std::vector<std::vector<std::string>> lineage() const;

    [[nodiscard]] std::optional<Area> absolute() const;

    void touched(Effect effect);

    void changing() const;

    void restyle();

    void attached(Node* parent);

    void detached();

    std::string id_;
    std::vector<std::string> classes_;
    States states_;
    Node* parent_ = nullptr;
    Document* document_ = nullptr;
    Children children_{*this};
    Style style_;
    bool styled_ = false;
    bool sizing_ = true;
    bool arranging_ = true;
    bool placed_ = false;
    std::optional<std::pair<Limits, Size>> sized_;
    Area area_;
    std::uint64_t arranged_in_ = 0;
    std::vector<std::pair<std::type_index, std::shared_ptr<detail::Slot>>> slots_;
    bool enabled_ = true;
    bool focusable_ = false;
    Share share_;
};

template <typename T>
class Property {
public:
    Property(Node& owner, Effect effect, T initial) : owner_(owner), effect_(effect), held_(std::move(initial)) {}

    Property(const Property&) = delete;
    Property& operator=(const Property&) = delete;

    void operator=(T given) {
        if (held_ == given) return;

        owner_.touched(effect_);
        held_ = std::move(given);
        owner_.changed(this);
    }

    [[nodiscard]] const T& operator*() const { return held_; }

    [[nodiscard]] const T* operator->() const { return &held_; }

private:
    Node& owner_;
    Effect effect_;
    T held_;
};

template <typename T, typename... Given>
T& Children::add(Given&&... given) {
    std::shared_ptr<T> made = std::make_shared<T>(std::forward<Given>(given)...);
    T& kept = *made;

    add(std::move(made));

    return kept;
}

}
