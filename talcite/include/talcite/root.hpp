#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "talcite/blocks.hpp"
#include "talcite/node.hpp"
#include "talcite/skin.hpp"

namespace talcite::ui {

class Root : public Node {
public:
    Root();

    ~Root() override;

    [[nodiscard]] const Skin& skin() const { return skin_; }

    void skin(Skin given);

    [[nodiscard]] Colors colors() const { return skin_.colors(); }

    void colors(Colors given);

    [[nodiscard]] bool unicode() const { return skin_.unicode(); }

    void unicode(bool given);

    [[nodiscard]] Blocks blocks() const { return blocks_; }

    void blocks(Blocks given);

    [[nodiscard]] Size cell() const { return cell_; }

    void cell(Size given);

    [[nodiscard]] std::optional<Spot> caret() const;

    [[nodiscard]] std::optional<std::chrono::milliseconds> due() const override;

    [[nodiscard]] Node* focus() const { return focus_; }

    void focus(Node* one);

    bool deal(KeyEvent& what);

    bool deal(PasteEvent& what);

    bool deal(SizeEvent& what);

    bool deal(MouseEvent& what);

    bool deal(WheelEvent& what);

    bool deal(ClickEvent& what);

    bool deal(DoubleClickEvent& what);

    [[nodiscard]] bool clicking() const { return clicking_; }

    void settle();

private:
    friend class Children;
    friend class Node;

    Skin skin_;
    Blocks blocks_ = Blocks::basic;
    Size cell_;

    Node* under(Spot where);

    void minded(const MouseEvent& what);

    // Doręczanie liczy się, bo w jego trakcie nie wolno nikogo zwolnić. Licznik, a nie znacznik,
    // bo uchwyt ma prawo doręczyć kolejne zdarzenie w środku pierwszego.
    template <std::derived_from<Event> One>
    bool sent(Node& one, One& what) {
        ++busy_;

        bool made = false;

        try {
            made = one.trigger(what);
        } catch (...) {
            released();

            throw;
        }

        released();

        return made;
    }

    void released();

    void linger(std::shared_ptr<Node> one);

    void losing(const Node* one);

    Node* focus_ = nullptr;
    std::vector<std::shared_ptr<Node>> dying_;
    int busy_ = 0;

    Spot began_;
    Mouse pressing_ = Mouse::none;

    Spot waiting_;
    Mouse clicked_ = Mouse::none;
    Mods marked_;
    bool clicking_ = false;
};

}
