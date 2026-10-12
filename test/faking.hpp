#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/clipboard.hpp"
#include "talcite/console.hpp"
#include "talcite/picture.hpp"
#include "talcite/tls.hpp"

namespace faking {

// A fake image provider: it does not decode GIF and does not pretend to. From a GIF file it takes
// only what lies in the header without decompressing: the screen size from bytes 6-9 and the
// delays from the control blocks 21 f9 04. It returns as many frames as there are blocks, each a
// uniform canvas of a different shade. A delay of up to one hundredth is raised to a hundred
// milliseconds, as the real decoder does. Anything else is refused with a message, and a zero size
// is refused with a separate one, so that the widget and seam trials check the mechanics rather
// than LZW, which lives in talcite-adapters.
class Images : public talcite::ImageProvider {
public:
    // The provider's name.
    [[nodiscard]] std::string_view name() const override { return "fake images"; }

    // Hands each frame to `each` as described above; returns an empty string on success or the
    // refusal message.
    [[nodiscard]] std::string decode(const talcite::Bytes& given, const talcite::Taking& each) override {
        if (given.size() < 13 || std::to_integer<char>(given[0]) != 'G' ||
            std::to_integer<char>(given[1]) != 'I' || std::to_integer<char>(given[2]) != 'F')
            return "not a GIF the fake would read";

        const int wide = std::to_integer<int>(given[6]) | (std::to_integer<int>(given[7]) << 8);
        const int high = std::to_integer<int>(given[8]) | (std::to_integer<int>(given[9]) << 8);

        if (wide == 0 || high == 0) return "empty screen";

        std::vector<std::chrono::milliseconds> delays;

        for (std::size_t at = 0; at + 5 < given.size(); ++at)
            if (std::to_integer<std::uint8_t>(given[at]) == 0x21 &&
                std::to_integer<std::uint8_t>(given[at + 1]) == 0xF9 &&
                std::to_integer<std::uint8_t>(given[at + 2]) == 0x04) {
                const int hundredths = std::to_integer<int>(given[at + 4]) |
                                       (std::to_integer<int>(given[at + 5]) << 8);

                delays.emplace_back(hundredths <= 1 ? 100 : hundredths * 10);
            }

        if (delays.empty()) delays.emplace_back(0);

        const std::size_t much = static_cast<std::size_t>(wide) * static_cast<std::size_t>(high) * 4;

        // A canvas above the ceiling is declared but not allocated: the fake hands over an empty
        // buffer with a large size, so that a trial can check that the seam cuts off such a
        // provider before anyone looks at the bytes. A real provider refuses by itself, from the
        // header, before it decompresses anything.
        if (much / 4 > talcite::ceiling) {
            (void)each(talcite::Bytes{}, {wide, high}, delays.front());

            return {};
        }

        for (std::size_t which = 0; which < delays.size(); ++which) {
            talcite::Bytes canvas(much, std::byte{static_cast<std::uint8_t>(60 + which * 40)});

            for (std::size_t at = 3; at < much; at += 4) canvas[at] = std::byte{255};

            if (!each(canvas, {wide, high}, delays[which])) return {};
        }

        return {};
    }
};

// A seam holding a new fake image provider.
[[nodiscard]] inline talcite::Seam<talcite::ImageProvider> images() {
    return talcite::Seam<talcite::ImageProvider>{std::make_shared<Images>()};
}

// A fake terminal. It has its own environment rather than the process's, set up from the start
// like a typical terminal, so a trial sets only what it cares about and the result does not
// depend on what the run was started from. Replies wait in the queue `replies`, one per hearing;
// those given at construction go whole or, when `piece` is given, that many bytes at a time.
// Everything written to it stays in `written`, and each hearing records its patience and whether
// it was handed a bell.
class Terminal : public talcite::Console {
public:
    // Queues `said` as one reply, or split into replies of `piece` bytes when `piece` is non-zero.
    explicit Terminal(std::string said = {}, std::size_t piece = 0) {
        if (said.empty()) return;

        if (piece == 0) {
            replies.push_back(std::move(said));

            return;
        }

        for (std::size_t at = 0; at < said.size(); at += piece) replies.push_back(said.substr(at, piece));
    }

    std::map<std::string, std::string, std::less<>> environment{{"TERM", "xterm-256color"},
                                                                {"LANG", "C.UTF-8"}};

    bool alive = true;
    bool sequences = false;
    std::optional<talcite::ui::Size> window;
    std::chrono::milliseconds twice{500};

    std::deque<std::string> replies;

    std::string written;
    int rounds = 0;
    int depth = 0;
    int entered = 0;
    int belled = 0;
    std::vector<std::optional<std::chrono::milliseconds>> patience;

    // The console's name.
    [[nodiscard]] std::string_view name() const override { return "fake terminal"; }

    // Looks the variable up in the fake's own `environment`.
    [[nodiscard]] std::optional<std::string> variable(std::string_view name) const override {
        const auto found = environment.find(name);

        if (found == environment.end()) return std::nullopt;

        return found->second;
    }

    // Whether the terminal is interactive, as set in `alive`.
    [[nodiscard]] bool lively() const override { return alive; }

    // Whether the terminal takes escape sequences, as set in `sequences`.
    [[nodiscard]] bool vt() const override { return sequences; }

    // The window size set in `window`, or nothing.
    [[nodiscard]] std::optional<talcite::ui::Size> size() const override { return window; }

    // The double-click interval set in `twice`.
    [[nodiscard]] std::chrono::milliseconds double_click() const override { return twice; }

    // Counts raw-mode nesting in `depth` and every entry in `entered`; leaving below zero is ignored.
    void raw(bool on) override {
        if (on) {
            ++depth;
            ++entered;
        } else if (depth > 0) {
            --depth;
        }
    }

    // Appends the bytes to `written`.
    void write(std::string_view bytes) override { written += bytes; }

    // A resume after a stop, reported once: as in the real console, asking clears the flag.
    bool comeback = false;

    // Returns `comeback` and clears it.
    [[nodiscard]] bool resumed() override {
        const bool made = comeback;

        comeback = false;

        return made;
    }

    // Records the round, its patience and any bell, then returns the next queued reply, or an
    // empty string when none is left.
    [[nodiscard]] std::string hear(std::optional<std::chrono::milliseconds> waited,
                                   const talcite::Bell* also) override {
        ++rounds;

        patience.push_back(waited);

        if (also != nullptr) ++belled;

        if (replies.empty()) return {};

        std::string made = std::move(replies.front());

        replies.pop_front();

        return made;
    }
};

// A new fake terminal with the given replies (see the constructor of `Terminal`).
[[nodiscard]] inline std::shared_ptr<Terminal> terminal(std::string said = {}, std::size_t piece = 0) {
    return std::make_shared<Terminal>(std::move(said), piece);
}

// A fake clipboard: one variable in memory. `refusing` imitates a clipboard that did not accept
// a copy; then nothing changes, as with a real refusal.
class Clipboard : public talcite::Clipboard {
public:
    std::optional<std::string> kept;
    bool refusing = false;
    int copies = 0;

    // The clipboard's name.
    [[nodiscard]] std::string_view name() const override { return "fake clipboard"; }

    // Counts the attempt in `copies` and keeps the text unless `refusing`; false when refused.
    [[nodiscard]] bool copy(std::string_view what) override {
        ++copies;

        if (refusing) return false;

        kept = std::string{what};

        return true;
    }

    // The kept text, or nothing when nothing was copied.
    [[nodiscard]] std::optional<std::string> paste() override { return kept; }
};

// A new, empty fake clipboard.
[[nodiscard]] inline std::shared_ptr<Clipboard> clipboard() { return std::make_shared<Clipboard>(); }

// This is not cryptography and does not pretend to be. It is a fake: it flips bits with a single
// byte, so that one can see the content really passes through the seam, and it exchanges a
// greeting, so that the pump has a handshake to carry out. The whole point is to test `TlsStream`
// (and what stands on it, such as the HTTP client) without any library. `refused` makes the client
// reject the handshake with the given reason, as an adapter that recognized it would.
class Turning : public talcite::net::TlsSession {
public:
    // `poison` replaces everything written with a poisoned record; `refused` fails the handshake.
    explicit Turning(bool poison = false, std::optional<talcite::net::TlsTrouble::Why> refused = std::nullopt)
        : poison_(poison), refused_(refused) {}

    // The handshake is done once the greeting has been both sent and heard.
    [[nodiscard]] bool ready() const override { return said_ && heard_; }

    // Takes ciphertext and unpacks complete records (a length byte, then the XOR-ed content): the
    // first must be the greeting, a poisoned one sets the error, a goodbye one marks the session
    // closed, the rest go to the plaintext.
    void push(talcite::Bytes given) override {
        came_.insert(came_.end(), given.begin(), given.end());

        for (;;) {
            if (came_.empty()) return;

            const std::size_t much = static_cast<std::size_t>(came_.front());

            if (came_.size() < much + 1) return;

            talcite::Bytes one(came_.begin() + 1, came_.begin() + 1 + static_cast<std::ptrdiff_t>(much));

            came_.erase(came_.begin(), came_.begin() + 1 + static_cast<std::ptrdiff_t>(much));

            for (std::byte& what : one) what ^= std::byte{0x5A};

            if (!heard_) {
                if (talcite::text(one) != "CZESC") {
                    amiss_ = "Greeting does not match.";

                    return;
                }

                heard_ = true;

                continue;
            }

            if (talcite::text(one) == "TRUCIZNA") {
                amiss_ = "Poison in the record.";

                return;
            }

            // The fake close_notify: the other side says goodbye.
            if (talcite::text(one) == "ZAMKNIJ") {
                closed_ = true;

                continue;
            }

            plain_.insert(plain_.end(), one.begin(), one.end());
        }
    }

    // The first request for ciphertext drives the handshake; without it nobody would start it.
    [[nodiscard]] talcite::Bytes pull() override {
        if (refused_.has_value()) {
            amiss_ = "Handshake rejected.";

            return {};
        }

        if (!said_) {
            said_ = true;

            framed(talcite::bytes("CZESC"));
        }

        talcite::Bytes made = std::move(out_);

        out_.clear();

        return made;
    }

    // Frames the plaintext into records of at most 255 bytes, or sends the poison instead.
    void write(talcite::Bytes given) override {
        if (poison_) given = talcite::bytes("TRUCIZNA");

        for (std::size_t at = 0; at < given.size(); at += 255) {
            const std::size_t much = std::min<std::size_t>(255, given.size() - at);

            framed(talcite::Bytes{given.begin() + static_cast<std::ptrdiff_t>(at),
                         given.begin() + static_cast<std::ptrdiff_t>(at + much)});
        }
    }

    // Hands over and clears the plaintext received so far.
    [[nodiscard]] talcite::Bytes read() override {
        talcite::Bytes made = std::move(plain_);

        plain_.clear();

        return made;
    }

    void close() override {}

    // Whether the other side sent its goodbye record.
    [[nodiscard]] bool closed() const override { return closed_; }

    // The error message, empty while nothing went wrong.
    [[nodiscard]] std::string error() const override { return amiss_; }

    // The refusal reason given at construction, otherwise `other`.
    [[nodiscard]] talcite::net::TlsTrouble::Why why() const override {
        return refused_.value_or(talcite::net::TlsTrouble::Why::other);
    }

private:
    // Appends one record to the outgoing ciphertext: its length byte, then the XOR-ed bytes.
    void framed(talcite::Bytes what) {
        out_.push_back(static_cast<std::byte>(what.size()));

        for (std::byte one : what) out_.push_back(one ^ std::byte{0x5A});
    }

    talcite::Bytes came_;
    talcite::Bytes plain_;
    talcite::Bytes out_;
    std::string amiss_;
    bool said_ = false;
    bool heard_ = false;
    bool poison_ = false;
    bool closed_ = false;
    std::optional<talcite::net::TlsTrouble::Why> refused_;
};

class Watching : public talcite::net::TlsProvider {
public:
    explicit Watching(bool poison = false, std::optional<talcite::net::TlsTrouble::Why> refused = std::nullopt)
        : poison_(poison), refused_(refused) {}

    // The provider's name.
    [[nodiscard]] std::string_view name() const override { return "flipping"; }

    // A fake client session with this provider's poison and refusal settings; the host is ignored.
    [[nodiscard]] std::unique_ptr<talcite::net::TlsSession> client(std::string) override {
        return std::make_unique<Turning>(poison_, refused_);
    }

    // A plain fake server session.
    [[nodiscard]] std::unique_ptr<talcite::net::TlsSession> server() override {
        return std::make_unique<Turning>();
    }

private:
    bool poison_ = false;
    std::optional<talcite::net::TlsTrouble::Why> refused_;
};

}
