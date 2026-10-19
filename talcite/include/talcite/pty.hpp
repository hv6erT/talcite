#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "talcite/area.hpp"
#include "talcite/bytes.hpp"
#include "talcite/child.hpp"
#include "talcite/promise.hpp"

namespace talcite {

class Pty {
public:
    Pty() = default;

    [[nodiscard]] static bool supported();

    [[nodiscard]] static Promise<Pty> start(Command what, ui::Size size);

    [[nodiscard]] bool held() const { return child_.held(); }

    [[nodiscard]] std::int64_t pid() const { return child_.pid(); }

    [[nodiscard]] Promise<std::size_t> write(Bytes what) const;

    [[nodiscard]] Promise<Bytes> read(std::size_t most) const;

    void resize(ui::Size size) const;

    [[nodiscard]] Promise<Exit> wait() const;

    void hang_up() const;

    void kill() const;

    void close();

private:
    explicit Pty(Child child) : child_(std::move(child)) {}

    Child child_;
};

}
