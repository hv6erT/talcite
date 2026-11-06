#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/fs.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"
#include "talcite/unpack.hpp"

namespace talcite::archive {

enum class Format : std::uint8_t { zip, tar, tar_gzip };

enum class Refusal : std::uint8_t { none, unsafe, encrypted, method, link };

struct Member {
    std::filesystem::path name;
    fs::Kind kind = fs::Kind::file;
    std::uintmax_t size = 0;
    std::uintmax_t packed = 0;
    std::chrono::sys_seconds modified{};
    std::optional<std::filesystem::perms> rights;
    Refusal refusal = Refusal::none;
    std::string stored;
};

struct Pick {
    std::filesystem::path inner;
    std::filesystem::path to;
};

class Archive {
public:
    struct Held;

    Archive() = default;

    explicit Archive(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const std::filesystem::path& where() const;

    [[nodiscard]] Format format() const;

    [[nodiscard]] const std::vector<Member>& members() const;

    [[nodiscard]] std::optional<std::size_t> found(const std::filesystem::path& inner) const;

    [[nodiscard]] std::vector<std::size_t> within(const std::filesystem::path& folder) const;

    [[nodiscard]] Promise<std::vector<Member>> extracted(std::vector<Pick> picks, fs::Conflict how = fs::Conflict::fail) const;

private:
    std::shared_ptr<Held> held_;
};

[[nodiscard]] std::optional<Format> format_of(const std::filesystem::path& name);

[[nodiscard]] Promise<Archive> opened(const std::filesystem::path& where, Seam<UnpackProvider> unpacking);

}
