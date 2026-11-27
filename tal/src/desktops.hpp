#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/desktop.hpp"
#include "tal/path.hpp"

namespace tal::desktops {

enum class Runnable : std::uint8_t { no, window, console, script };

struct Glob {
    int weight = 50;
    std::string type;
    std::string pattern;
    bool cased = false;
};

using Lists = std::map<std::string, std::vector<std::string>, std::less<>>;

struct Associations {
    Lists defaults;
    Lists added;
    Lists removed;
};

struct Entry {
    std::string name;
    std::string exec;
    std::string try_exec;
    std::string icon;
    std::vector<std::string> types;
    bool application = false;
    bool hidden = false;
    bool shown = true;
    bool terminal = false;
};

struct Environment {
    std::optional<std::string> home;
    std::optional<std::string> config_home;
    std::optional<std::string> config_dirs;
    std::optional<std::string> data_home;
    std::optional<std::string> data_dirs;
    std::optional<std::string> current;
};

struct Places {
    std::vector<Path> lists;
    std::vector<Path> applications;
    std::vector<Path> mime;
};

[[nodiscard]] std::string lowered(std::string_view text);

[[nodiscard]] std::vector<Glob> globs(std::string_view text);

[[nodiscard]] std::optional<std::string> type_of(std::string_view name, const std::vector<Glob>& known);

[[nodiscard]] Lists parents(std::string_view text);

[[nodiscard]] Associations associations(std::string_view text);

[[nodiscard]] Lists cached(std::string_view text);

[[nodiscard]] std::optional<Entry> entry(std::string_view text, std::string_view locale);

[[nodiscard]] std::optional<std::vector<std::string>> expanded(const Entry& one, std::string_view file, std::string_view location);

[[nodiscard]] Places places(const Environment& from);

[[nodiscard]] std::optional<std::uint16_t> subsystem(std::span<const std::byte> head);

[[nodiscard]] Runnable windows_runnable(std::string_view extension, std::span<const std::byte> head);

[[nodiscard]] Runnable posix_runnable(std::span<const std::byte> head, bool executable);

[[nodiscard]] Origin zone_of(std::string_view text);

[[nodiscard]] bool textual(std::span<const std::byte> head);

}
