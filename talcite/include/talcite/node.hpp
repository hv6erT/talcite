#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "talcite/area.hpp"
#include "talcite/handler.hpp"
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

    [[nodiscard]] bool contains(const Node* one) const;

    [[nodiscard]] const std::shared_ptr<Node>& operator[](std::size_t which) const {
        if (which >= inside_.size())
            throw std::out_of_range("talcite::ui::Children: nie ma takiego dziecka");

        return inside_[which];
    }

    [[nodiscard]] const std::shared_ptr<Node>& front() const {
        if (inside_.empty()) throw std::out_of_range("talcite::ui::Children: nie ma żadnego dziecka");

        return inside_.front();
    }

    [[nodiscard]] const std::shared_ptr<Node>& back() const {
        if (inside_.empty()) throw std::out_of_range("talcite::ui::Children: nie ma żadnego dziecka");

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

    [[nodiscard]] const std::string& role() const { return role_; }

    virtual void role(std::string given);

    [[nodiscard]] virtual int takes() const;

    [[nodiscard]] virtual bool accepts(const Node& one) const;

    void unfocus();

    [[nodiscard]] virtual int layer() const;

    [[nodiscard]] virtual bool focusable() const;

    [[nodiscard]] bool focused() const;

    virtual void focus();

    // **Klawisz aktywny to rozdanie, a nie pytanie o typ.** Węzeł podaje to, co niesie, swoim
    // potomkom i czeka, aż któryś weźmie. Pierwszy, który powie „tak", kończy przejście; kto chce
    // odpowiadać, przesłania `react(HotEvent<...>&)` albo wiesza `on<>`.
    //
    // **Zatrzymujemy się na potomku chętnym na ognisko**: co ma w środku, rozstrzyga on sam. Ta sama
    // granica rządzi wędrówką Taba, i z tego samego powodu — przycisk schowany w liście nie ma
    // odpowiadać na literę pisaną w tej liście.
    //
    // Chodzimy po **migawce**, a nie po żywym wektorze: uchwyt ma prawo dopiąć albo odpiąć dziecko
    // w reakcji, a wtedy bufor się przenosi i dalsze przejście czyta zwolnioną pamięć. Kopie
    // wskazań trzymają przy tym potomków przy życiu na czas rozdania.
    //
    // Zdarzenie składamy **osobno dla każdego potomka**, bo `trigger` zapisuje w nim, komu je
    // doręczono, i przy jednym egzemplarzu na całe przejście `at()` kłamałoby po pierwszym kroku.
    template <std::derived_from<Event> One>
    bool offer(One& what) {
        const std::vector<std::shared_ptr<Node>> these{inside_.begin(), inside_.end()};

        for (const std::shared_ptr<Node>& one : these) {
            if (offer(what, *one)) return true;

            if (one->focusable()) continue;

            if (one->offer(what)) return true;
        }

        return false;
    }

    View render(const Area& where);

    View render(const Area& where, const Area& seen);

    [[nodiscard]] virtual Size wants(Size room) const;

    [[nodiscard]] virtual std::optional<Spot> caret() const;

    [[nodiscard]] virtual std::optional<std::chrono::milliseconds> due() const;

protected:
    template <std::derived_from<Event> One>
    bool offer(One& what, Node& one) {
        HotEvent<One> hot{what};

        return one.trigger(hot);
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
};

}
