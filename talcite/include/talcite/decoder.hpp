#pragma once

#include <memory>
#include <string_view>

#include "talcite/root.hpp"

namespace talcite {

class Decoder {
public:
    Decoder();

    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;

    void fed(std::string_view bytes, ui::Root& onto);

    void flush(ui::Root& onto);

    [[nodiscard]] bool pending() const;

private:
    struct Held;

    std::unique_ptr<Held> held_;
};

}
