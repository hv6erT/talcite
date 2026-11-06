#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/archive.hpp"
#include "talcite/bytes.hpp"
#include "talcite/fs.hpp"
#include "talcite/seam.hpp"
#include "talcite/unpack.hpp"

namespace talcite::archive {

inline constexpr std::size_t members_most = 1000000;

struct Spot {
    std::uintmax_t at = 0;
    std::uintmax_t packed = 0;
    std::uint32_t crc = 0;
    std::uint16_t method = 0;
};

struct Archive::Held {
    std::filesystem::path where;
    Format format = Format::zip;
    Seam<UnpackProvider> unpacking;
    std::vector<Member> members;
    std::vector<Spot> spots;
};

struct Laid {
    std::filesystem::path name;
    bool unsafe = false;
};

[[nodiscard]] Laid laid(std::string_view stored);

[[nodiscard]] bool utf8_whole(std::string_view raw);

[[nodiscard]] std::string from_cp437(std::string_view raw);

[[nodiscard]] std::string from_latin1(std::string_view raw);

[[nodiscard]] std::uint32_t crc32(std::uint32_t crc, std::span<const std::byte> bytes);

[[noreturn]] void damaged(const std::string& said, const std::filesystem::path& where);

class Source {
public:
    Source(std::shared_ptr<fs::File::Held> file, std::filesystem::path where, std::uintmax_t size, std::unique_ptr<Unpacking> gzip);

    [[nodiscard]] std::uintmax_t at() const { return at_; }

    [[nodiscard]] std::size_t read(std::span<std::byte> into);

    void taken(std::span<std::byte> into);

    void skipped(std::uintmax_t much);

private:
    std::shared_ptr<fs::File::Held> file_;
    std::filesystem::path where_;
    std::uintmax_t size_ = 0;
    std::uintmax_t raw_ = 0;
    std::uintmax_t at_ = 0;
    std::unique_ptr<Unpacking> gzip_;
    Bytes waiting_;
    std::size_t used_ = 0;
};

void zip_read(Archive::Held& held, const fs::File::Held& file, std::uintmax_t size);

void tar_read(Archive::Held& held, Source& source);

}
