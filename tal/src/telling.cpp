#include "telling.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/text.hpp"

#include "hexing.hpp"

// Notifications through the terminal, which shows them on the machine the user sits at, also over
// ssh. Three sequences of OSC are in use, and a terminal ignores those it does not know without a
// word, so the one to send is chosen by the terminal's name: OSC 99 of kitty (a title and a body,
// the urgency, both texts in base64), OSC 777 `notify` of foot and urxvt's extension (a title and a
// body, parted by a semicolon), and OSC 9 of iTerm2, WezTerm and ghostty (one text). A terminal
// named otherwise gets none, and the notice goes to the system.
//
// The terminal is the application's, and only its loop writes to it, between frames: a notice told
// from any thread waits in `Terminal` and the loop writes it at its next turn. While the
// application has stepped aside for another program, notices wait for it to come back. When the
// application ends, what waits is written and every later notice goes to the system.
//
// The texts reach the terminal inside the sequence, so nothing that could end it may stay in them:
// bytes that are not UTF-8 are repaired, and control characters, C0, DEL and C1, are left out.
namespace tal::telling {

namespace {

constexpr std::string_view bel = "\x07";
constexpr std::string_view st = "\x1b\\";

// Whether `what` occurs anywhere in `where`.
[[nodiscard]] bool holds(std::string_view where, std::string_view what) { return where.find(what) != std::string_view::npos; }

// The text in base64.
[[nodiscard]] std::string encoded(std::string_view text) { return crypto::base64(bytes(text)); }

// The value in hexadecimal.
[[nodiscard]] std::string hexadecimal(std::uint64_t value) {
    std::string made;

    do {
        made.insert(made.begin(), hex::lower[value & 0x0F]);
        value >>= 4;
    } while (value != 0);

    return made;
}

// A mark of this notifier among those of every program in the terminal: the moment it was made in
// nanoseconds and its place among the notifiers of this program, both in hexadecimal, so that
// kitty's notices of two programs, or of two applications one after another, never share an
// identifier.
[[nodiscard]] std::string stamped() {
    static std::atomic<std::uint64_t> made{0};
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    return hexadecimal(static_cast<std::uint64_t>(now)) + "." + hexadecimal(++made);
}

// The urgency as kitty counts it: 0, 1 or 2.
[[nodiscard]] int level(Urgency urgency) {
    switch (urgency) {
    case Urgency::low: return 0;
    case Urgency::critical: return 2;
    case Urgency::normal: break;
    }

    return 1;
}

}

// The sequence a terminal of the name `named` (as `Capabilities::named` gives it) shows: kitty's
// own, foot's 777, OSC 9 for iTerm2, WezTerm and ghostty, none for any other.
Way way(std::string_view named) {
    if (holds(named, "kitty")) return Way::kitty;
    if (holds(named, "foot")) return Way::notify;
    if (holds(named, "iTerm") || holds(named, "WezTerm") || holds(named, "ghostty")) return Way::nine;

    return Way::none;
}

// The text with what could end a sequence taken out: bytes that are not UTF-8 repaired, control
// characters left out but for line breaks and tabs, which become spaces unless `lines` keeps line
// breaks.
std::string shown(std::string_view text, bool lines) {
    const std::string whole = text::repaired(text);
    std::string made;

    for (std::size_t at = 0; at < whole.size();) {
        const std::size_t from = at;
        const std::optional<char32_t> one = text::decoded(whole, at);

        if (!one) break;

        if (*one == U'\n' && lines) made += '\n';
        else if (*one == U'\n' || *one == U'\t') made += ' ';
        else if (*one >= 0x20 && (*one < 0x7F || *one > 0x9F)) made.append(whole, from, at - from);
    }

    return made;
}

// The sequence that shows `notice` the way `how`, nothing for none. `id` tells kitty's notices
// apart, so that one never adds to another or takes its place. A text of OSC 9 that starts with
// digits and a semicolon gets a space before it, or ConEmu's heirs (ghostty, WezTerm) would read a
// command.
std::string sequence(const Notice& notice, Way how, std::string_view id) {
    switch (how) {
    case Way::none: return {};
    case Way::nine: {
        const std::string title = shown(notice.title, false);
        const std::string body = shown(notice.body, false);
        std::string both = title.empty() ? body : body.empty() ? title : title + ": " + body;
        std::size_t digits = 0;

        while (digits < both.size() && both[digits] >= '0' && both[digits] <= '9') ++digits;

        if (digits > 0 && digits < both.size() && both[digits] == ';') both.insert(0, " ");

        return "\x1b]9;" + both + std::string{bel};
    }
    case Way::notify: {
        std::string title = shown(notice.title, false);

        for (char& one : title)
            if (one == ';') one = ',';

        return "\x1b]777;notify;" + title + ";" + shown(notice.body, false) + std::string{bel};
    }
    case Way::kitty: {
        const std::string named = "i=" + std::string{id};
        const std::string urgency = ":u=" + std::to_string(level(notice.urgency));
        const std::string body = shown(notice.body, true);
        std::string made = "\x1b]99;" + named + ":d=" + (body.empty() ? "1" : "0") + urgency + ":e=1;" + encoded(shown(notice.title, true)) +
                           std::string{st};

        if (!body.empty()) made += "\x1b]99;" + named + ":d=1:p=body:e=1;" + encoded(body) + std::string{st};

        return made;
    }
    }

    return {};
}

// What a terminal notifier shares with the work it posts to the loop, which may outlive it: the
// sequences waiting, and the loop, none once the application closed it.
struct Terminal::Shared {
    Port<Console> console;
    Way how;
    std::shared_ptr<Notifier> otherwise;

    std::mutex guard;
    Executor* loop = nullptr;
    bool aside = false;
    std::deque<std::string> waiting;
    std::string marked = stamped();
    std::uint64_t told = 0;

    // Over the application's console and loop.
    Shared(Port<Console> with, Way given, std::shared_ptr<Notifier> other, Executor& on)
        : console(std::move(with)), how(given), otherwise(std::move(other)), loop(&on) {}

    // Writes what waits, on the loop, unless the terminal is another program's now.
    void written() {
        std::deque<std::string> now;

        {
            const std::lock_guard<std::mutex> kept{guard};

            if (aside) return;

            now.swap(waiting);
        }

        for (const std::string& one : now) console->write(one);
    }
};

// A notifier that writes to the application's terminal on `loop` the way `how`, and hands a notice
// to `otherwise` when the terminal has no way or the application has ended.
Terminal::Terminal(Port<Console> console, Way how, std::shared_ptr<Notifier> otherwise, Executor& loop)
    : shared_(std::make_shared<Shared>(std::move(console), how, std::move(otherwise), loop)) {}

// Puts the notice's sequence among those waiting and asks the loop to write them, or hands it on.
Task<void> Terminal::tell(Notice notice) {
    std::shared_ptr<Notifier> other;

    {
        const std::lock_guard<std::mutex> kept{shared_->guard};

        if (shared_->how != Way::none && shared_->loop != nullptr) {
            const std::string id = shared_->marked + "-" + std::to_string(++shared_->told);

            shared_->waiting.push_back(sequence(notice, shared_->how, id));
            shared_->loop->post([shared = shared_]() { shared->written(); });

            co_return;
        }

        other = shared_->otherwise;
    }

    co_await other->tell(std::move(notice));
}

// Called by the application on its loop as it ends: writes what waits, and from now on hands every
// notice on. An application that ends while it stands aside (which `App::Pause`, holding it, does
// not let happen) leaves the terminal to the other program, and what waits is not written.
void Terminal::closed() {
    {
        const std::lock_guard<std::mutex> kept{shared_->guard};

        shared_->loop = nullptr;
    }

    shared_->written();
}

// Called by the application on its loop as it steps aside for another program (`aside`), which
// holds what is told, and as it comes back, which writes what waited.
void Terminal::away(bool aside) {
    {
        const std::lock_guard<std::mutex> kept{shared_->guard};

        shared_->aside = aside;
    }

    shared_->written();
}

// The notifier a notice goes to when the terminal shows none.
std::shared_ptr<Notifier> Terminal::otherwise() const { return shared_->otherwise; }

}
