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

    [[nodiscard]] Graphics graphics() const { return graphics_; }

    void graphics(Graphics given);

    [[nodiscard]] std::optional<Spot> caret() const override;

    [[nodiscard]] std::optional<std::chrono::milliseconds> due() const override;

    [[nodiscard]] Node* focused_node() const { return focus_; }

    void move_focus(Node* one);

    bool dispatch(KeyEvent& what);

    bool dispatch(PasteEvent& what);

    bool dispatch(SizeEvent& what);

    bool dispatch(MouseEvent& what);

    bool dispatch(WheelEvent& what);

    bool dispatch(ClickEvent& what);

    bool dispatch(DoubleClickEvent& what);

    [[nodiscard]] std::chrono::milliseconds double_click() const { return double_click_; }

    void double_click(std::chrono::milliseconds given) { double_click_ = given; }

    void capture(Node& one);

protected:
    [[nodiscard]] virtual bool foresee(KeyEvent& what);

private:
    friend class Children;
    friend class Node;

    Skin skin_;
    Blocks blocks_ = Blocks::basic;
    Size cell_;
    Graphics graphics_ = Graphics::none;

    Node* under(Spot where);

    void minded(const MouseEvent& what);

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
    std::weak_ptr<Node> held_;

    std::chrono::milliseconds double_click_{400};
    std::optional<std::chrono::steady_clock::time_point> first_click_;
    Spot waiting_;
    Mouse clicked_ = Mouse::none;
};

}
