#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"

namespace talcite::net {

using Number = std::array<std::byte, 12>;

enum class Sort : std::uint8_t { asking, told, amiss, telling };

// Sprawy, czyli co wiadomość ma załatwić. Rodzaj wiadomości składa się ze sprawy i klasy, a te
// dwie rzeczy są w liczbie przemieszane bitami — stąd osobne wyliczenie zamiast gołych liczb.
// Nazwa nie brzmi Way — tak nazywała się wtedy droga do rozmówcy w peer.hpp, dziś `Candidate`.
enum class Task : std::uint16_t {
    binding = 0x001,
    allocate = 0x003,
    refresh = 0x004,
    sending = 0x006,
    data = 0x007,
    permission = 0x008,
    channel = 0x009
};

struct Message {
    Sort sort = Sort::asking;
    Task task = Task::binding;
    Number number{};
    Address seen;
    Address lent;
    Address peer;
    Bytes data;
    int code = 0;
    int lifetime = 0;
    std::string said;
    std::string realm;
    std::string nonce;
};

[[nodiscard]] Number numbered();

[[nodiscard]] Bytes asked(const Number& number);

[[nodiscard]] Bytes begun(Task task, Sort sort, const Number& number);

void placing(Bytes& made, std::uint16_t kind, const Address& where, bool xored);

void wording(Bytes& made, std::uint16_t kind, std::uint32_t what);

void giving(Bytes& made, const Bytes& what);

[[nodiscard]] Bytes wrapped(std::uint16_t channel, const Bytes& what);

[[nodiscard]] bool unwrapped(const Bytes& what, std::uint16_t& channel, Bytes& data);

[[nodiscard]] Bytes told(const Number& number, const Address& seen);

[[nodiscard]] bool understood(const Bytes& what, Message& made);

[[nodiscard]] std::uint32_t tallied(const Bytes& what, std::size_t much);

void texting(Bytes& made, std::uint16_t kind, const std::string& said);

void stamping(Bytes& made, const Bytes& key);

void printing(Bytes& made);

[[nodiscard]] bool trusted(const Bytes& what, const Bytes& key);

[[nodiscard]] Bytes keyed(const std::string& user, const std::string& realm,
                          const std::string& word);

}
