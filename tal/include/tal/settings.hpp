#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tal/json.hpp"
#include "tal/path.hpp"
#include "tal/task.hpp"

namespace tal {

using Records = std::vector<std::map<std::string, std::string>>;

template <typename T>
struct Setting {
    std::string_view road;
};

class Settings {
public:
    Settings(std::string_view builtin, Path file);

    ~Settings();

    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    [[nodiscard]] Task<void> read();

    [[nodiscard]] Task<void> write() const;

    [[nodiscard]] bool damaged() const { return damaged_; }

    [[nodiscard]] Task<void> repair();

    [[nodiscard]] Task<void> restore();

    template <typename T>
    [[nodiscard]] T get(Setting<T> one) const;

    template <typename T>
    [[nodiscard]] T builtin(Setting<T> one) const;

    template <typename T>
    void set(Setting<T> one, const T& value);

    template <typename T>
    void reset(Setting<T> one);

    template <typename T>
    [[nodiscard]] bool changed(Setting<T> one) const;

    void reset();

    [[nodiscard]] const Path& where() const { return where_; }

private:
    struct Gate;

    json::Value builtin_;
    json::Value mine_;
    Path where_;
    bool damaged_ = false;
    std::shared_ptr<Gate> gate_;
};

}
