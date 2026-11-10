#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"
#include "talcite/xml.hpp"

namespace talcite::net {

[[nodiscard]] Promise<Address> gateway();

[[nodiscard]] Promise<Address> leaving(Address toward);

enum class Carrier : std::uint8_t { tcp, udp };

enum class Opener : std::uint8_t { pcp, pmp, upnp };

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
    Address discovery{"239.255.255.250", 1900};
    Seam<Codec<xml::Document>> xml;
    std::string label = "talcite";
    Patience patience{10000};
    Patience turn{1750};
};

class Router {
public:
    struct Held;

    Router() = default;

    explicit Router(Routing routing);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Promise<Opening> open(Carrier carrier, std::uint16_t inside, std::chrono::seconds lasting) const;

    [[nodiscard]] Promise<void> close(Opening one) const;

private:
    std::shared_ptr<Held> held_;
};

}
