#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/net.hpp"
#include "tal/port.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"

namespace tal::net {

enum class Mapping { unknown, alike, symmetric };

class Stun {
public:
    explicit Stun(std::vector<Address> servers, std::chrono::milliseconds each = std::chrono::milliseconds{7500});

    [[nodiscard]] Task<Address> seen() const;

    [[nodiscard]] Task<Mapping> mapping() const;

private:
    std::vector<Address> servers_;
    std::chrono::milliseconds each_;
};

struct Sighting {
    Address from;
    Bytes payload;

    friend bool operator==(const Sighting&, const Sighting&) = default;
};

class Beacon {
public:
    static constexpr std::size_t widest_tag = 64;
    static constexpr std::size_t widest_payload = 1024;

    Beacon() = default;

    [[nodiscard]] static Task<Beacon> opened(std::uint16_t port);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Address mine() const;

    void offer(Bytes tag, Bytes payload) const;

    void withdraw(const Bytes& tag) const;

    void close();

    [[nodiscard]] static Task<std::vector<Sighting>> sought(std::uint16_t port, Bytes tag, std::chrono::milliseconds lasting, std::vector<Address> to = {});

    struct Held;

private:
    explicit Beacon(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

[[nodiscard]] Task<Address> gateway();

[[nodiscard]] Task<Address> leaving(Address toward);

enum class Carrier { tcp, udp };

enum class Opener { pcp, pmp, upnp };

struct Opening {
    Opener by = Opener::pcp;
    Carrier carrier = Carrier::tcp;
    std::uint16_t inside = 0;
    Address outside;
    std::chrono::seconds lasting{0};
};

struct Routing {
    Address gateway;
    Address mine;
    std::chrono::milliseconds turn{1750};
    Address discovery{"239.255.255.250", 1900};
    std::string label = "tal";
    std::chrono::milliseconds patience{10000};
};

class Router {
public:
    explicit Router(Routing how);

    [[nodiscard]] Task<Opening> open(Carrier carrier, std::uint16_t inside, std::chrono::seconds lasting) const;

    [[nodiscard]] Task<void> close(Opening one) const;

    struct Held;

private:
    std::shared_ptr<Held> held_;
};

enum class Side { calling, answering };

struct Keying {
    Bytes sending;
    Bytes receiving;
};

class Handshake {
public:
    virtual ~Handshake() = default;

    Handshake(const Handshake&) = delete;
    Handshake& operator=(const Handshake&) = delete;

    [[nodiscard]] virtual std::optional<Bytes> said() = 0;

    virtual void heard(const Bytes& one) = 0;

    [[nodiscard]] virtual bool ready() const = 0;

    [[nodiscard]] virtual const Keying& keys() const = 0;

protected:
    Handshake() = default;
};

[[nodiscard]] std::shared_ptr<Handshake> keyed(Side side, Bytes key, std::string context, Port<crypto::Cipher> cipher = {});

[[nodiscard]] std::shared_ptr<Handshake> cpace(Side side, Bytes password, std::string context, Port<crypto::Cipher> cipher = {});

class Sealed {
public:
    static constexpr std::size_t longest = std::size_t{4} << 20;

    Sealed() = default;

    [[nodiscard]] static Task<Sealed> opened(Stream one, std::shared_ptr<Handshake> how, Port<crypto::Cipher> cipher = {});

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Task<void> send(Bytes what) const;

    [[nodiscard]] Task<Bytes> received() const;

    void close();

    struct Held;

private:
    explicit Sealed(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

}
