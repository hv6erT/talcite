#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/capabilities.hpp"
#include "tal/clock.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/node.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/task.hpp"
#include "tal/text.hpp"

namespace tal {

struct Display {
    Size screen{};
    Colors colors = Colors::rgb;
    bool unicode = true;
    text::Rendering rendering{};
    Graphics graphics = Graphics::none;
    Blocks blocks = Blocks::basic;
    Size cell{};

    friend bool operator==(const Display&, const Display&) = default;
};

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
    explicit Document(Port<Clock> time = {}, Port<Processes> processes = {});

    void sheet(StyleSheet given);

    [[nodiscard]] const StyleSheet& sheet() const { return sheet_; }

    void display(Display shown);

    [[nodiscard]] bool due() const;

    const Surface& frame();

    [[nodiscard]] const Tally& tally() const { return tally_; }

    [[nodiscard]] std::uint64_t framed() const { return frames_; }

    void double_click(std::chrono::milliseconds most);

    [[nodiscard]] Node* focused() const { return focus_; }

    void confine(Node* within);

    [[nodiscard]] Node* confined() const { return confined_; }

    [[nodiscard]] std::optional<Caret> caret() const override;

    [[nodiscard]] Timer after(std::chrono::milliseconds wait, Fn<Timer> what);

    void key(Key one);

    void mouse(Press press, MouseButton button, Spot screen, Mods mods = {}, int steps = 0, Axis axis = Axis::vertical);

    void paste(std::string text);

    void follow(std::string address);

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> alarm() const;

    void ring();

private:
    friend class Node;
    friend class Children;

    enum class Stage { idle, styling, laying, settling, painting };

    void delivered(Event& one, Node& target);

    [[nodiscard]] Node& under(Spot screen);

    [[nodiscard]] Node& reachable(Node& one);

    [[nodiscard]] Node& keyed();

    void drawn(Node& one, Area box, Area seen, Area clip, Area part);

    void moved(Node* to);

    [[nodiscard]] Node* following(const Node* from, bool back, const Node* skipped);

    void forgotten(Node& gone);

    void hovered(Node* now);

    void clicked(Press press, MouseButton button, Spot screen, Mods mods);

    void damaged(Area part);

    void styled(Node& one);

    void painted(Node& one, Spot origin, Area clip, Area part);

    [[nodiscard]] Task<void> opened(std::string address);

    void inspected();

    Port<Clock> time_;
    Port<Processes> processes_;
    std::optional<Scope> scope_;
    StyleSheet sheet_;
    Display display_{.colors = Colors::mono, .unicode = false};
    Surface surface_{{0, 0}};
    std::vector<Area> damage_;
    bool restyling_ = true;
    Stage stage_ = Stage::idle;
    std::uint64_t frames_ = 0;
    std::chrono::milliseconds double_click_{400};
    Tally tally_;
    std::vector<std::shared_ptr<Node>> arranged_;
    Node* focus_ = nullptr;
    Node* captured_ = nullptr;
    Node* confined_ = nullptr;
    std::weak_ptr<Node> deferred_;
    std::vector<Node*> hover_;
    Spot began_;
    MouseButton pressing_ = MouseButton::none;
    std::optional<std::chrono::steady_clock::time_point> first_click_;
    Spot waiting_;
    MouseButton clicked_ = MouseButton::none;
    std::vector<std::weak_ptr<Timer::State>> timers_;
    std::uint64_t timed_ = 0;
    Hooks own_;
};

}
