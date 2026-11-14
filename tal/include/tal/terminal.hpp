#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/clipboard.hpp"
#include "tal/emulator.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/node.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/surface.hpp"
#include "tal/task.hpp"
#include "tal/views.hpp"

namespace tal {

struct Moved : Event {
    explicit Moved(std::string given) : folder(std::move(given)) {}

    std::string folder;
};

struct Marked : Event {
    explicit Marked(Staged given) : stage(given) {}

    Staged stage;
};

struct Switched : Event {
    explicit Switched(bool given) : alternate(given) {}

    bool alternate;
};

struct Titled : Event {
    explicit Titled(std::string given) : title(std::move(given)) {}

    std::string title;
};

struct Rang : Event {};

struct Finished : Event {
    explicit Finished(Exit given) : exit(given) {}

    Exit exit;
};

[[nodiscard]] Bytes keyed(Key key, const Modes& modes);

class Terminal : public Scrollable {
public:
    explicit Terminal(Command program, std::size_t kept = 5000, Port<Processes> processes = {}, Port<Clipboard> clipboard = {});

    ~Terminal() override;

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    Property<bool> prompt{*this, Effect::paint, true};

    void run();

    [[nodiscard]] bool running() const { return running_; }

    [[nodiscard]] const Emulator& screen() const { return emulator_; }

    void send(std::string_view text);

    void submit(std::string_view line);

    bool paste(std::string_view text);

    [[nodiscard]] bool idle() const;

    void hang_up();

    [[nodiscard]] std::string selected() const;

    void unselect();

    [[nodiscard]] Task<bool> copy();

    [[nodiscard]] std::size_t extent() const override;

    [[nodiscard]] std::size_t window() const override;

    [[nodiscard]] std::size_t top() const override;

    void top(std::size_t row) override;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

    [[nodiscard]] std::optional<Caret> caret() const override;

private:
    struct At {
        std::uint64_t row = 0;
        int column = 0;

        friend auto operator<=>(const At&, const At&) = default;
    };

    [[nodiscard]] Task<void> started();

    [[nodiscard]] Task<void> listened();

    [[nodiscard]] Task<void> waited();

    [[nodiscard]] Task<void> lingered();

    [[nodiscard]] Task<void> poured();

    [[nodiscard]] Task<void> held(std::uint64_t which);

    void heard(const Bytes& got);

    void exited(Exit exit);

    void drained();

    void ended(Exit exit);

    bool write(Bytes what);

    void slid(long long rows);

    void typed(KeyEvent& what);

    void moused(MouseEvent& what);

    void reported(const MouseEvent& what);

    [[nodiscard]] int bottom() const;

    [[nodiscard]] int deepest() const;

    [[nodiscard]] int first() const;

    [[nodiscard]] At placed(Spot at) const;

    [[nodiscard]] std::optional<std::pair<At, At>> span() const;

    void worded(At at);

    void scrolled();

    Command command_;
    Emulator emulator_;
    Port<Processes> processes_;
    Port<Clipboard> clipboard_;
    Process process_;
    bool running_ = false;
    bool starting_ = false;
    bool submitted_ = false;
    bool drained_ = false;
    bool writing_ = false;
    bool holding_ = false;
    std::uint64_t hold_ = 0;
    std::optional<Exit> exit_;
    int scrolled_ = 0;
    Bytes outbox_;
    std::optional<At> anchor_;
    std::optional<At> head_;
    bool selecting_ = false;
    Hooks hooks_;
    Scope scope_;
};

}
