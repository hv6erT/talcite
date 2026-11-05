#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/stream.hpp"

#if !defined(TALCITE_POLL)
#if defined(_WIN32)
#define TALCITE_IOCP 1
#elif defined(__linux__)
#define TALCITE_EPOLL 1
#endif
#endif

namespace talcite::net {

#ifdef _WIN32
using Handle = std::uintptr_t;

inline constexpr Handle nothing = static_cast<Handle>(~0ull);
#else
using Handle = int;

inline constexpr Handle nothing = -1;
#endif

enum class Kind : std::uint8_t { joining, meeting, getting, putting, fetching, sending };

struct Doing : std::enable_shared_from_this<Doing> {
    virtual ~Doing() = default;

    virtual void done(std::size_t much, int code) = 0;

    virtual void late() = 0;

    std::shared_ptr<void> kept;
    Handle held = nothing;
    Kind kind = Kind::getting;
    Bytes buffer;
    std::size_t much = 0;
    Address where;
    Handle made = nothing;
    bool cut = false;
    std::chrono::steady_clock::time_point until{};
    bool ever = false;
};

void submit(std::shared_ptr<Doing> one);

[[nodiscard]] bool withdraw(const std::shared_ptr<Doing>& one);

void forget(Handle one);

void retiring(Handle one);

[[nodiscard]] bool outward(Kind kind);

[[nodiscard]] bool stepped(Doing& one);

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

[[nodiscard]] Stream greeting(Handle one, Address from);

[[nodiscard]] int filling(const Address& from, void* into);

[[nodiscard]] Address facing(const void* from);

}
