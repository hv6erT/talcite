#pragma once

#include <string>

#include "talcite/shape.hpp"
#include "talcite/span.hpp"

namespace talcite::ui {

class Status : public Span {
public:
    explicit Status(std::string text = {}, SpanShape shape = {});

    [[nodiscard]] Size wants(Size room) const override;
};

}
