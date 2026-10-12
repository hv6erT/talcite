#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"
#include "talcite/stream.hpp"

namespace talcite::net {

class TlsTrouble : public Trouble {
public:
    enum class Why : std::uint8_t { ciphers, untrusted, expired, misnamed, other };

    TlsTrouble(Why why, const std::string& said, std::string host = {}, Address where = {});

    [[nodiscard]] Why why() const { return why_; }

    [[nodiscard]] const std::string& host() const { return host_; }

private:
    Why why_;
    std::string host_;
};

class TlsSession {
public:
    virtual ~TlsSession() = default;

    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    TlsSession(TlsSession&&) = delete;
    TlsSession& operator=(TlsSession&&) = delete;

    [[nodiscard]] virtual bool ready() const = 0;

    virtual void push(Bytes given) = 0;

    [[nodiscard]] virtual Bytes pull() = 0;

    virtual void write(Bytes given) = 0;

    [[nodiscard]] virtual Bytes read() = 0;

    virtual void close() = 0;

    [[nodiscard]] virtual bool closed() const = 0;

    [[nodiscard]] virtual std::string error() const = 0;

    [[nodiscard]] virtual TlsTrouble::Why why() const { return TlsTrouble::Why::other; }

protected:
    TlsSession() = default;
};

class TlsProvider : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "TLS"; }

    [[nodiscard]] virtual std::unique_ptr<TlsSession> client(std::string host) = 0;

    [[nodiscard]] virtual std::unique_ptr<TlsSession> server() = 0;
};

class TlsStream {
public:
    struct Held;

    TlsStream() = default;

    explicit TlsStream(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    [[nodiscard]] static Promise<TlsStream> client(Stream one, Seam<TlsProvider> tls,
                                                   std::string host, Patience patience);

    [[nodiscard]] static Promise<TlsStream> server(Stream one, Seam<TlsProvider> tls,
                                                   Patience patience);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const Address& peer() const;

    [[nodiscard]] const Address& mine() const;

    [[nodiscard]] Promise<Bytes> read(std::size_t most, Patience patience) const;

    [[nodiscard]] Promise<void> write(Bytes what, Patience patience) const;

    [[nodiscard]] bool cut() const;

    void close();

private:
    std::shared_ptr<Held> held_;
};

}
