#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/fs.hpp"
#include "tal/path.hpp"
#include "tal/progress.hpp"
#include "tal/task.hpp"

namespace tal::archive {

enum class Format { zip, tar, tar_gzip };

enum class Refusal { none, unsafe, encrypted, method, link };

struct Member {
    Path name;
    fs::Type type = fs::Type::file;
    std::uint64_t size = 0;
    std::uint64_t packed = 0;
    std::chrono::system_clock::time_point modified;
    std::optional<std::uint16_t> rights;
    Refusal refusal = Refusal::none;
    std::string stored;
};

struct Pick {
    Path inner;
    Path to;
};

class Archive {
public:
    struct Held;

    Archive() = default;

    explicit Archive(std::shared_ptr<Held> held);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] const Path& where() const;

    [[nodiscard]] Format format() const;

    [[nodiscard]] const std::vector<Member>& members() const;

    [[nodiscard]] std::optional<std::size_t> found(const Path& inner) const;

    [[nodiscard]] std::vector<std::size_t> within(const Path& folder) const;

    [[nodiscard]] Task<std::vector<Member>> extracted(std::vector<Pick> picks, fs::Conflict how = fs::Conflict::fail, Progress* progress = nullptr) const;

private:
    std::shared_ptr<Held> held_;
};

[[nodiscard]] std::optional<Format> format_of(const Path& name);

[[nodiscard]] Task<Archive> opened(Path where);

}
