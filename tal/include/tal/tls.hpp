#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "tal/bytes.hpp"
#include "tal/port.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

namespace tal::net {

class Session {
public:
    Session() = default;
    virtual ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] virtual bool ready() const = 0;

    virtual void push(Bytes given) = 0;

    [[nodiscard]] virtual Bytes pull() = 0;

    virtual void write(Bytes given) = 0;

    [[nodiscard]] virtual Bytes read() = 0;

    virtual void close() = 0;

    [[nodiscard]] virtual bool closed() const = 0;

    [[nodiscard]] virtual std::optional<Trouble> failed() const = 0;
};

struct Credentials {
    Bytes identity;
    std::string password;
    std::string trusted;
};

class Tls : public Adapter {
public:
    static constexpr std::string_view kind = "tls";

    [[nodiscard]] static std::shared_ptr<Tls> builtin();

    [[nodiscard]] static std::shared_ptr<Tls> builtin(Credentials given);

    [[nodiscard]] virtual std::unique_ptr<Session> client(std::string host) = 0;

    [[nodiscard]] virtual std::unique_ptr<Session> server() = 0;
};

[[nodiscard]] Task<Stream> secured(Stream plain, std::string host, Port<Tls> tls = {});

[[nodiscard]] Task<Stream> serving(Stream plain, Port<Tls> tls);

}
