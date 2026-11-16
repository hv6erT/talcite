#include "turning.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/trouble.hpp"

// A TLS the trials write, for the trials of what runs over TLS: records of one length byte and the
// content mangled, a greeting each way for the handshake, a goodbye record for the close, and a
// poisoned record or a refused handshake on demand.
namespace tal::testing {

namespace {

using tal::Bytes;

// A session the trials write, as `Turning` was in talcite.
class Turning final : public tal::net::Session {
public:
    // A client's session, or a server's (`answering`); `poison` turns everything written into a
    // poisoned record, and `refused` fails the handshake with that kind of trouble.
    Turning(bool answering, bool poison, std::optional<tal::Kind> refused) : answering_(answering), poison_(poison), refused_(refused) {}

    // Ready once both greetings went.
    [[nodiscard]] bool ready() const override { return said_ && heard_; }

    // Unpacks the whole records in what came: the greeting first, then a poison, a goodbye or
    // plaintext.
    void push(Bytes given) override {
        came_.insert(came_.end(), given.begin(), given.end());

        while (!came_.empty()) {
            const auto much = std::to_integer<std::size_t>(came_.front());

            if (came_.size() < much + 1) return;

            Bytes one(came_.begin() + 1, came_.begin() + 1 + static_cast<std::ptrdiff_t>(much));

            came_.erase(came_.begin(), came_.begin() + 1 + static_cast<std::ptrdiff_t>(much));

            for (std::byte& each : one) each ^= std::byte{0x5a};

            const std::string said = tal::text::of(one);

            if (!heard_) {
                if (said != "CZESC") failed_ = tal::Trouble{tal::Kind::broken, "The greeting does not match."};

                heard_ = true;
            } else if (said == "TRUCIZNA") {
                failed_ = tal::Trouble{tal::Kind::broken, "Poison in the record."};
            } else if (said == "ZAMKNIJ") {
                closed_ = true;
            } else {
                plain_.insert(plain_.end(), one.begin(), one.end());
            }
        }
    }

    // What there is to send; the first call sends the greeting, a server's after the client's.
    [[nodiscard]] Bytes pull() override {
        if (refused_) {
            failed_ = tal::Trouble{*refused_, "The handshake is refused."};

            return {};
        }

        if (!said_ && (!answering_ || heard_)) {
            said_ = true;
            framed(tal::bytes("CZESC"));
        }

        return std::exchange(out_, {});
    }

    // Records of at most 255 bytes, or the poison.
    void write(Bytes given) override {
        if (poison_) given = tal::bytes("TRUCIZNA");

        for (std::size_t at = 0; at < given.size(); at += 255)
            framed(Bytes{given.begin() + static_cast<std::ptrdiff_t>(at), given.begin() + static_cast<std::ptrdiff_t>(std::min(at + 255, given.size()))});
    }

    // The plaintext so far.
    [[nodiscard]] Bytes read() override { return std::exchange(plain_, {}); }

    // The goodbye.
    void close() override { framed(tal::bytes("ZAMKNIJ")); }

    // Whether the other side said goodbye.
    [[nodiscard]] bool closed() const override { return closed_; }

    // What went wrong, if anything.
    [[nodiscard]] std::optional<tal::Trouble> failed() const override { return failed_; }

private:
    // Appends the record of `what`.
    void framed(const Bytes& what) {
        out_.push_back(static_cast<std::byte>(what.size()));

        for (const std::byte one : what) out_.push_back(one ^ std::byte{0x5a});
    }

    Bytes came_;
    Bytes plain_;
    Bytes out_;
    std::optional<tal::Trouble> failed_;
    bool said_ = false;
    bool heard_ = false;
    bool closed_ = false;
    bool answering_;
    bool poison_;
    std::optional<tal::Kind> refused_;
};

// The adapter of `Turning`.
class Turned final : public tal::net::Tls {
public:
    // Clients with `poison` and `refused`; servers plain.
    explicit Turned(bool poison = false, std::optional<tal::Kind> refused = std::nullopt) : poison_(poison), refused_(refused) {}

    // Its name.
    [[nodiscard]] std::string_view name() const override { return "turning"; }

    // A client's session.
    [[nodiscard]] std::unique_ptr<tal::net::Session> client(std::string host) override {
        (void)host;

        return std::make_unique<Turning>(false, poison_, refused_);
    }

    // A server's session.
    [[nodiscard]] std::unique_ptr<tal::net::Session> server() override { return std::make_unique<Turning>(true, false, std::nullopt); }

private:
    bool poison_;
    std::optional<tal::Kind> refused_;
};

}

// The adapter of a `Turned`, with `poison` and `refused` for its clients.
tal::Port<tal::net::Tls> turned(bool poison, std::optional<tal::Kind> refused) {
    return tal::Port<tal::net::Tls>{std::make_shared<Turned>(poison, refused)};
}

}
