#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "tal/bytes.hpp"
#include "tal/meta.hpp"
#include "tal/task.hpp"

namespace tal::meta::tagging {

inline constexpr std::size_t most_text = std::size_t{1} << 20;
inline constexpr std::size_t most_block = std::size_t{16} << 20;
inline constexpr std::size_t most_packet = std::size_t{64} << 20;
inline constexpr std::size_t most_picture = std::size_t{64} << 20;
inline constexpr std::size_t window = std::size_t{64} << 10;

[[nodiscard]] std::size_t windowed();

void windowed(std::size_t least);

class Walk {
public:
    Walk(Reader& from, std::uint64_t size, std::uint64_t at, std::uint64_t end);

    [[nodiscard]] static Task<Walk> over(Reader& from);

    [[nodiscard]] std::uint64_t at() const { return at_; }

    [[nodiscard]] std::uint64_t end() const { return end_; }

    [[nodiscard]] std::uint64_t left() const { return at_ < end_ ? end_ - at_ : 0; }

    [[nodiscard]] std::uint64_t size() const { return size_; }

    [[nodiscard]] bool spoiled() const { return spoiled_; }

    void spoil() { spoiled_ = true; }

    [[nodiscard]] Reader& reader() const { return *from_; }

    [[nodiscard]] Task<void> need(std::size_t much);

    [[nodiscard]] Task<Bytes> take(std::size_t much);

    [[nodiscard]] std::uint8_t u8();

    [[nodiscard]] std::uint16_t be16();

    [[nodiscard]] std::uint32_t be24();

    [[nodiscard]] std::uint32_t be32();

    [[nodiscard]] std::uint64_t be64();

    [[nodiscard]] std::uint16_t le16();

    [[nodiscard]] std::uint32_t le32();

    [[nodiscard]] std::uint64_t le64();

    [[nodiscard]] Bytes bytes(std::size_t much);

    [[nodiscard]] std::string chars(std::size_t much);

    [[nodiscard]] bool ahead(std::string_view magic);

    void skip(std::uint64_t much);

    void seek(std::uint64_t to);

    [[nodiscard]] Walk part(std::uint64_t much);

private:
    [[nodiscard]] const std::byte* near(std::size_t much);

    Reader* from_;
    std::uint64_t size_;
    std::uint64_t at_;
    std::uint64_t end_;
    bool spoiled_ = false;
    std::shared_ptr<const Bytes> held_;
    std::span<const std::byte> window_;
    std::uint64_t window_at_ = 0;
};

struct Gathered {
    explicit Gathered(Description& given, std::optional<std::size_t> picked = std::nullopt, Bytes* taken = nullptr)
        : into(given), wanted(picked), bytes(taken) {}

    Description& into;
    std::optional<std::size_t> wanted;
    Bytes* bytes = nullptr;
    bool found = false;
    std::string refused;
    std::string codec;

    void field(Scheme scheme, std::string key, Value value);

    [[nodiscard]] bool pictured(Picture one);

    void took(Bytes data);

    [[nodiscard]] bool done() const { return found || !refused.empty(); }

    [[nodiscard]] bool wanting() const { return wanted && !found && *wanted == into.pictures.size(); }

    void damaged() { into.damaged = true; }
};

[[nodiscard]] std::string latin1(std::span<const std::byte> given);

[[nodiscard]] std::string utf8(std::span<const std::byte> given);

[[nodiscard]] std::string utf16(std::span<const std::byte> given, bool big);

[[nodiscard]] std::string utf16(std::span<const std::byte> given);

[[nodiscard]] std::string upper(std::string ascii);

[[nodiscard]] std::uint32_t be32_of(std::span<const std::byte> given, std::size_t at);

[[nodiscard]] Role role_of(std::uint32_t kind);

[[nodiscard]] std::string image_type(std::span<const std::byte> head);

[[nodiscard]] std::string image_named(std::string_view said);

[[nodiscard]] std::optional<std::uint16_t> depth_in(std::string_view codec, std::span<const std::byte> data);

[[nodiscard]] Transfer transfer_of(std::uint64_t code);

[[nodiscard]] std::string vfw_codec(std::string fourcc);

[[nodiscard]] std::string acm_codec(std::uint16_t tag);

[[nodiscard]] Task<std::uint64_t> id3v2(Walk& walk, Gathered& out);

[[nodiscard]] Task<bool> id3v2_at(Walk& walk);

[[nodiscard]] Task<std::uint64_t> id3v1(Reader& from, std::uint64_t size, std::uint64_t end, Gathered& out);

[[nodiscard]] Task<std::uint64_t> ape(Reader& from, std::uint64_t size, std::uint64_t end, Gathered& out);

[[nodiscard]] Task<std::uint64_t> ape_ahead(Walk& walk, Gathered& out);

[[nodiscard]] Task<bool> mpeg(Walk& walk, Gathered& out);

[[nodiscard]] Task<void> flac(Walk& walk, Gathered& out);

[[nodiscard]] Task<void> streaminfo(Walk& block, Gathered& out);

[[nodiscard]] Task<void> vorbis(Walk& block, Gathered& out);

[[nodiscard]] Task<void> picture_block(Walk& block, Gathered& out);

[[nodiscard]] Task<void> ogg(Reader& from, std::uint64_t size, Gathered& out);

[[nodiscard]] Task<void> mp4(Reader& from, std::uint64_t size, Gathered& out);

[[nodiscard]] Task<void> wavpack(Walk& walk, Gathered& out);

[[nodiscard]] Task<void> matroska(Reader& from, std::uint64_t size, Gathered& out);

[[nodiscard]] Task<void> avi(Reader& from, std::uint64_t size, Gathered& out);

[[nodiscard]] Task<void> gathered(Reader& from, Gathered& out);

}
