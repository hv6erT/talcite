#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/stream.hpp"

// Wybór zaplecza stoi tu, żeby wszystkie pliki zgadzały się co do jednego. TALCITE_POLL wymusza
// drogę przenośną wszędzie — bez tego zaplecze zapasowe nie miałoby jak przejść testów na
// maszynie, na której akurat jest coś lepszego.
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

// Zgłoszenie leżące w centrali. Centrala **wykonuje** jego krok — sama wie, jak to zrobić na
// danym systemie — a zgłoszenie tylko odbiera wynik: tyle bajtów, ten numer błędu. Dzięki temu
// czytanie, przyjmowanie i wysyłanie nie wiedzą nic o poll, epoll ani IOCP, a nowe zaplecze
// dokłada się bez ruszania ani jednej z nich.
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

// Zdjęcie **jednego** zgłoszenia, bez tykania gniazda i bez tykania drugiego kierunku. To nie jest
// forget: tamten zdejmuje gniazdo w całości, zrywa rozmowę i odrzuca wszystko, co na nim wisiało.
// Prawda znaczy „naprawdę wyszło i done() ani late() już na nim nie pobiegnie" — i tylko wtedy
// wołający ma prawo posprzątać po sobie sam.
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

void nagling(Handle one, bool given);

void sharing(Handle one);

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
