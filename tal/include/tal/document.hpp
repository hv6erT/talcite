#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/clock.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/node.hpp"
#include "tal/port.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"

namespace tal {

struct Tally {
    std::uint64_t styled = 0;
    std::uint64_t measured = 0;
    std::uint64_t arranged = 0;
    std::uint64_t painted = 0;

    friend bool operator==(const Tally&, const Tally&) = default;
};

class Timer {
public:
    Timer() = default;

    ~Timer();

    Timer(Timer&& other) noexcept;
    Timer& operator=(Timer&& other) noexcept;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void again(std::chrono::milliseconds wait);

    [[nodiscard]] bool waiting() const;

private:
    friend class Document;

    struct State;

    explicit Timer(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;
};

class Document : public Node {
public:
    explicit Document(Port<Clock> time = {});

    void sheet(StyleSheet given);

    [[nodiscard]] const StyleSheet& sheet() const { return sheet_; }

    void display(Size screen, Colors colors, bool unicode);

    [[nodiscard]] bool due() const;

    const Surface& frame();

    [[nodiscard]] const Tally& tally() const { return tally_; }

    [[nodiscard]] Node* focused() const { return focus_; }

    [[nodiscard]] Timer after(std::chrono::milliseconds wait, Fn<Timer> what);

    void key(Key one);

    void mouse(Press press, Button button, Spot screen, Mods mods = {}, int steps = 0);

    void paste(std::string text);

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> alarm() const;

    void ring();

private:
    friend class Node;
    friend class Children;

    enum class Stage { idle, styling, laying, settling, painting };

    void delivered(Event& one, Node& target);

    [[nodiscard]] Node& under(Spot screen);

    [[nodiscard]] Node& reachable(Node& one);

    void moved(Node* to);

    [[nodiscard]] Node* following(const Node* from, bool back, const Node* skipped);

    void forgotten(Node& gone);

    void hovered(Node* now);

    void clicked(Press press, Button button, Spot screen, Mods mods);

    void damaged(Area part);

    void styled(Node& one);

    void painted(Node& one, Spot origin, Area clip, Area part);

    Port<Clock> time_;
    StyleSheet sheet_;
    Size screen_;
    Colors colors_ = Colors::mono;
    bool unicode_ = false;
    Surface surface_{{0, 0}};
    std::vector<Area> damage_;
    bool restyling_ = true;
    Stage stage_ = Stage::idle;
    std::uint64_t frames_ = 0;
    Tally tally_;
    std::vector<std::shared_ptr<Node>> arranged_;
    Node* focus_ = nullptr;
    Node* captured_ = nullptr;
    std::vector<Node*> hover_;
    Spot began_;
    Button pressing_ = Button::none;
    std::optional<std::chrono::steady_clock::time_point> first_click_;
    Spot waiting_;
    Button clicked_ = Button::none;
    std::vector<std::weak_ptr<Timer::State>> timers_;
    std::uint64_t timed_ = 0;
    Hooks own_;
};

}
