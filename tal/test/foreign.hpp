#pragma once

#include <cstdint>
#include <memory>

#include "tal/bytes.hpp"
#include "tal/pixels.hpp"

namespace tal::testing {

[[nodiscard]] std::shared_ptr<media::Decoding> foreign();

[[nodiscard]] Bytes foreign_image(media::Size size, std::uint8_t tone);

}
