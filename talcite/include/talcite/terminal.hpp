#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "talcite/bytes.hpp"
#include "talcite/child.hpp"
#include "talcite/emulator.hpp"
#include "talcite/event.hpp"
#include "talcite/node.hpp"
#include "talcite/promise.hpp"
#include "talcite/pty.hpp"

namespace talcite::ui {

class FolderEvent : public Event {
public:
    explicit FolderEvent(std::string folder) : folder_(std::move(folder)) {}

    [[nodiscard]] const std::string& folder() const { return folder_; }

private:
    std::string folder_;
};

class MarkEvent : public Event {
public:
    explicit MarkEvent(ShellMark mark) : mark_(mark) {}

    [[nodiscard]] const ShellMark& mark() const { return mark_; }

private:
    ShellMark mark_;
};

class SwitchEvent : public Event {
public:
    explicit SwitchEvent(bool alternate) : alternate_(alternate) {}

    [[nodiscard]] bool alternate() const { return alternate_; }

private:
    bool alternate_ = false;
};

class TitleEvent : public Event {
public:
    explicit TitleEvent(std::string title) : title_(std::move(title)) {}

    [[nodiscard]] const std::string& title() const { return title_; }

private:
    std::string title_;
};

class EndEvent : public Event {
public:
    explicit EndEvent(Exit exit) : exit_(exit) {}

    [[nodiscard]] const Exit& exit() const { return exit_; }

private:
    Exit exit_;
};

class Terminal : public Node {
public:
    explicit Terminal(Command what, Size size = {80, 24}, std::size_t kept = 5000);

    ~Terminal() override;

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void run();

    [[nodiscard]] bool running() const { return running_; }

    [[nodiscard]] const Emulator& screen() const { return emulator_; }

    void send(std::string_view text);

    void submit(std::string_view line);

    [[nodiscard]] bool idle() const;

    void hang_up();

    [[nodiscard]] bool prompt_shown() const { return prompt_shown_; }

    void prompt_shown(bool given) { prompt_shown_ = given; }

    [[nodiscard]] int scrolled() const { return scrolled_; }

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] std::optional<Spot> caret() const override;

    [[nodiscard]] CaretShape caret_shape() const override;

    using Node::caret_shape;

    [[nodiscard]] static Bytes keyed(Key key, const Modes& modes);

protected:
    View paint(Size given, Area seen) override;

    using Node::handle;

    bool handle(KeyEvent& what) override;

    bool handle(PasteEvent& what) override;

    bool handle(ClickEvent& what) override;

    bool handle(MouseEvent& what) override;

    bool handle(WheelEvent& what) override;

    bool handle(FocusEvent& what) override;

    bool handle(UnfocusEvent& what) override;

private:
    [[nodiscard]] Promise<void> listened();

    void heard(const Bytes& got);

    void exited(const Exit& exit);

    void drained();

    void ended(const Exit& exit);

    void write(Bytes what);

    void flushed();

    [[nodiscard]] int bottom() const;

    [[nodiscard]] int deepest() const;

    Command command_;
    Emulator emulator_;
    Pty pty_;
    bool running_ = false;
    bool starting_ = false;
    bool prompt_shown_ = true;
    bool submitted_ = false;
    int scrolled_ = 0;
    Bytes outbox_;
    bool writing_ = false;
    std::optional<Promise<void>> started_;
    std::optional<Promise<void>> reading_;
    std::optional<Promise<void>> waiting_;
    std::optional<Promise<void>> lingering_;
    std::optional<Exit> exit_;
    bool drained_ = false;
    std::optional<Promise<void>> sending_;
};

}
