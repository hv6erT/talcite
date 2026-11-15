#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/net.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#if !defined(TAL_NET_POLL) && !defined(TAL_NET_KQUEUE)
#if defined(_WIN32)
#define TAL_NET_IOCP 1
#elif defined(__linux__)
#define TAL_NET_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define TAL_NET_KQUEUE 1
#else
#define TAL_NET_POLL 1
#endif
#endif

#if defined(_WIN32) && !defined(TAL_NET_IOCP)
#error "Windows waits for its sockets through IOCP only."
#endif

namespace tal::net::netting {

#ifdef _WIN32
using Handle = std::uintptr_t;

inline constexpr Handle nothing = static_cast<Handle>(~0ULL);
#else
using Handle = int;

inline constexpr Handle nothing = -1;
#endif

enum class Step : std::uint8_t { joining, meeting, getting, putting, fetching, sending };

class Socket {
public:
    explicit Socket(Handle one);
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    void close();

    [[nodiscard]] Handle now() const { return which_.load(); }

private:
    std::atomic<Handle> which_;
};

class Doing {
public:
    Doing(std::shared_ptr<Socket> given, Step step);

    Doing(const Doing&) = delete;
    Doing& operator=(const Doing&) = delete;

    void done(std::size_t moved, int why);

    void waited(const std::shared_ptr<detail::Chain>& chain, std::coroutine_handle<> handle);

    bool left(bool taken);

    [[nodiscard]] bool gone();

    [[nodiscard]] bool closed() const;

    const std::shared_ptr<Socket> socket;
    const Handle held;
    const Step kind;
    Bytes buffer;
    std::size_t much = 0;
    Address where;
    Handle made = nothing;
    bool cut = false;
    std::size_t got = 0;
    int code = 0;

private:
    void abandoned();

    std::mutex guard_;
    std::shared_ptr<detail::Chain> chain_;
    std::coroutine_handle<> handle_;
    bool armed_ = false;
    bool gone_ = false;
    bool over_ = false;
};

void submit(std::shared_ptr<Doing> one);

[[nodiscard]] bool withdraw(const std::shared_ptr<Doing>& one);

void forget(Handle one);

void retiring(Handle one);

[[nodiscard]] std::size_t watching();

[[nodiscard]] std::size_t turns();

class Awaited {
public:
    explicit Awaited(std::shared_ptr<Doing> one) : one_(std::move(one)) {}

    Awaited(const Awaited&) = delete;
    Awaited& operator=(const Awaited&) = delete;

    ~Awaited();

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template <typename P>
    void await_suspend(std::coroutine_handle<P> handle) {
        const std::shared_ptr<Doing> one = one_;

        one->waited(detail::chain_of(handle), handle);
        submit(one);
    }

    void await_resume() noexcept { finished_ = true; }

private:
    std::shared_ptr<Doing> one_;
    bool finished_ = false;
};

void starting();

[[nodiscard]] int amiss();

[[nodiscard]] std::string spoken(int code);

[[nodiscard]] ::tal::Kind kind_of(int code);

[[noreturn]] void failed(const std::string& said, int code, const Address& where = {});

[[nodiscard]] int aborted();

void ending(Handle one);

[[nodiscard]] Handle opened(Family family, bool stream);

[[nodiscard]] bool joined(Handle one, const Address& where);

[[nodiscard]] int settling(Handle one);

[[nodiscard]] Address named(Handle one, bool peer);

void sharing(Handle one);

void broadcasting(Handle one, bool given);

[[nodiscard]] std::optional<bool> kindred(Handle one);

[[nodiscard]] bool listening(Handle one, const Address& where, int waiting);

[[nodiscard]] bool bound(Handle one, const Address& where);

[[nodiscard]] Address taken(Handle one, Handle& made);

[[nodiscard]] long getting(Handle one, void* into, std::size_t much);

[[nodiscard]] long putting(Handle one, const void* from, std::size_t much);

[[nodiscard]] long fetching(Handle one, void* into, std::size_t much, Address& from, bool& cut);

[[nodiscard]] long sending(Handle one, const void* from, std::size_t much, const Address& to);

[[nodiscard]] bool again();

[[nodiscard]] bool underway();

void shutting(Handle one);

[[nodiscard]] int filling(const Address& from, void* into);

[[nodiscard]] Address facing(const void* from);

[[nodiscard]] std::string pathed(const Path& where);

void removed(const Address& where);

#ifdef _WIN32
[[nodiscard]] int refusal(const Address& where);
#endif

[[nodiscard]] std::vector<Address> looked(const std::string& host, std::uint16_t port, Family family);

[[nodiscard]] std::vector<Address> ordered(std::vector<Address> all);

[[nodiscard]] Task<Connection> tried(std::vector<Address> all);

}
