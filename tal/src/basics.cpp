#include "tal/bytes.hpp"
#include "tal/clock.hpp"
#include "tal/log.hpp"
#include "tal/port.hpp"
#include "tal/trouble.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>

namespace tal {

// The bytes of a text, as they are.
Bytes bytes(std::string_view text) {
    Bytes made(text.size());

    for (std::size_t at = 0; at < text.size(); ++at) made[at] = static_cast<std::byte>(text[at]);

    return made;
}

// The text of some bytes, as they are.
std::string text::of(std::span<const std::byte> given) {
    std::string made(given.size(), '\0');

    for (std::size_t at = 0; at < given.size(); ++at) made[at] = static_cast<char>(given[at]);

    return made;
}

// A trouble of the given kind, with the sentence a person may read, the place it concerns (a path
// or an address, as text) and the system's own code when there was one.
Trouble::Trouble(Kind kind, std::string said, std::string where, int code)
    : kind_(kind), said_(std::move(said)), where_(std::move(where)), code_(code) {}

// A deadline passed: the work did not finish in the time it was given.
Overdue::Overdue() : Trouble(Kind::overdue, "The time ran out.") {}

// Cancelled work: not an error, so it stands outside `Trouble` and nobody reports it as one.
const char* Cancelled::what() const noexcept { return "The work was cancelled."; }

// The kind as a word, for logs and trials.
std::string_view spoken(Kind kind) {
    switch (kind) {
        case Kind::missing: return "missing";
        case Kind::denied: return "denied";
        case Kind::exists: return "exists";
        case Kind::busy: return "busy";
        case Kind::full: return "full";
        case Kind::refused: return "refused";
        case Kind::broken: return "broken";
        case Kind::malformed: return "malformed";
        case Kind::unsupported: return "unsupported";
        case Kind::overdue: return "overdue";
        case Kind::other: return "other";
    }

    return "other";
}

namespace {

// The last entries of the log, kept for a crash report: at most this many.
constexpr std::size_t kept_entries = 200;

// The log's memory: the last entries, under one lock.
struct Logbook {
    std::mutex guard;
    std::deque<std::string> entries;
};

// The process's one logbook, made on first use and never destroyed, so that logging from a thread
// that outlives static destruction still finds it.
Logbook& logbook() {
    static auto* const made = new Logbook;

    return *made;
}

// The level as the prefix of an entry.
std::string_view prefix(Level level) {
    switch (level) {
        case Level::debug: return "debug: ";
        case Level::info: return "info: ";
        case Level::warning: return "warning: ";
        case Level::error: return "error: ";
    }

    return "";
}

}

// Writes an entry: into the memory of the last entries, and to the error stream when `TAL_LOG` is
// set or the build is a debug build and the entry is an error.
void log(Level level, std::string_view said) {
    std::string entry{prefix(level)};

    entry += said;

#ifndef NDEBUG
    const bool loud = level == Level::error || std::getenv("TAL_LOG") != nullptr;
#else
    const bool loud = std::getenv("TAL_LOG") != nullptr;
#endif

    if (loud) std::fprintf(stderr, "%s\n", entry.c_str());

    Logbook& book = logbook();
    const std::lock_guard<std::mutex> kept{book.guard};

    book.entries.push_back(std::move(entry));

    if (book.entries.size() > kept_entries) book.entries.pop_front();
}

// The last entries of the log, oldest first.
std::vector<std::string> recent() {
    Logbook& book = logbook();
    const std::lock_guard<std::mutex> kept{book.guard};

    return {book.entries.begin(), book.entries.end()};
}

namespace {

// How many guards of `tal::testing` expect unhandled failures now, and how many came so far.
std::atomic<int> tolerating{0};
std::atomic<std::uint64_t> unhandled_seen{0};

}

namespace detail {

// A failure nobody handled: it goes to the log, and a debug build stops the program there, unless
// a trial said it expects one (`tolerate`).
void unhandled(std::string_view said) {
    log(Level::error, said);
    unhandled_seen.fetch_add(1);

#ifndef NDEBUG
    if (tolerating.load() == 0) std::abort();
#endif
}

// Changes the number of guards expecting unhandled failures by `change`.
void tolerate(int change) { tolerating.fetch_add(change); }

// How many unhandled failures came since the program started.
std::uint64_t unhandled_count() { return unhandled_seen.load(); }

}

// The program's defaults for ports, made on first use and never destroyed: a port constructed
// during static destruction still finds them.
Defaults& defaults() {
    static auto* const made = new Defaults;

    return *made;
}

// A port of the given kind has no adapter: neither the program's default nor a built-in one.
void unbound(std::string_view kind) {
    throw Trouble{Kind::missing, "No adapter is given for the port \"" + std::string{kind} + "\"."};
}

namespace {

// The system's clocks: the steady one for durations and deadlines, the calendar for dates.
class SystemClock : public Clock {
public:
    // The adapter's name.
    [[nodiscard]] std::string_view name() const override { return "system"; }

    // The steady clock of the system.
    [[nodiscard]] std::chrono::steady_clock::time_point now() const override { return std::chrono::steady_clock::now(); }

    // The calendar clock of the system.
    [[nodiscard]] std::chrono::system_clock::time_point calendar() const override { return std::chrono::system_clock::now(); }
};

}

// The built-in clock: the system's, one for the whole program.
std::shared_ptr<Clock> Clock::builtin() {
    static const auto made = std::make_shared<SystemClock>();

    return made;
}

// A clock standing still at the system's current time until advanced; trials move it by hand.
ManualClock::ManualClock() : now_(std::chrono::steady_clock::now()), calendar_(std::chrono::system_clock::now()) {}

// The current steady time of the manual clock.
std::chrono::steady_clock::time_point ManualClock::now() const {
    const std::lock_guard<std::mutex> kept{guard_};

    return now_;
}

// The current calendar time of the manual clock.
std::chrono::system_clock::time_point ManualClock::calendar() const {
    const std::lock_guard<std::mutex> kept{guard_};

    return calendar_;
}

// Moves both clocks forward by `much`; timers fall due at the next turn of the loops that use it.
void ManualClock::advance(std::chrono::steady_clock::duration much) {
    const std::lock_guard<std::mutex> kept{guard_};

    now_ += much;
    calendar_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(much);
}

}
