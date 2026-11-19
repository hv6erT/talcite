#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/archive.hpp"
#include "tal/bytes.hpp"
#include "tal/fs.hpp"
#include "tal/path.hpp"
#include "tal/task.hpp"

#include "inflating.hpp"

namespace tal::archive {

namespace archiving {

inline constexpr std::size_t members_most = 1000000;

struct Spot {
    std::uint64_t at = 0;
    std::uint64_t packed = 0;
    std::uint32_t crc = 0;
    std::uint16_t method = 0;
};

struct Laid {
    Path name;
    bool unsafe = false;
};

[[nodiscard]] Laid laid(std::string_view stored);

[[nodiscard]] std::chrono::system_clock::time_point instant(std::int64_t seconds);

[[nodiscard]] std::string from_cp437(std::string_view raw);

[[nodiscard]] std::string from_latin1(std::string_view raw);

[[noreturn]] void damaged(const std::string& said, const Path& where);

void fits(const fs::Volume& volume, std::uint64_t bytes, std::uint64_t largest, const Path& to);

class Source {
public:
    Source(fs::File file, Path where, std::uint64_t size, bool gzip);

    [[nodiscard]] std::uint64_t at() const { return at_; }

    [[nodiscard]] Task<std::size_t> read(std::span<std::byte> into);

    [[nodiscard]] Task<void> taken(std::span<std::byte> into);

    [[nodiscard]] Task<void> skipped(std::uint64_t much);

private:
    fs::File file_;
    Path where_;
    std::uint64_t size_ = 0;
    std::uint64_t raw_ = 0;
    std::uint64_t at_ = 0;
    std::optional<inflating::Inflater> gzip_;
    Bytes waiting_;
    std::size_t used_ = 0;
};

}

struct Archive::Held {
    Path where;
    Format format = Format::zip;
    std::vector<Member> members;
    std::vector<archiving::Spot> spots;
};

namespace archiving {

[[nodiscard]] Task<void> zip_read(Archive::Held& held, fs::File file, std::uint64_t size);

[[nodiscard]] Task<void> tar_read(Archive::Held& held, Source& source);

}

}
