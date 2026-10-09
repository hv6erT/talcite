#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "talcite/bytes.hpp"
#include "talcite/pixels.hpp"
#include "talcite/seam.hpp"

namespace talcite {

inline constexpr std::size_t ceiling = 8'000'000;

class ImageProvider : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "Image"; }

    [[nodiscard]] virtual std::string decode(const Bytes& given, const Taking& each) = 0;
};

template <>
class Seam<ImageProvider> {
public:
    Seam() = default;

    explicit Seam(std::shared_ptr<ImageProvider> given) : provider_(std::move(given)) {}

    [[nodiscard]] bool bound() const { return provider_ != nullptr; }

    [[nodiscard]] ImageProvider& provider() const;

    void decoded(const Bytes& given, const Taking& each) const;

    [[nodiscard]] Pixels decoded(const Bytes& given, std::size_t most = 0) const;

private:
    std::shared_ptr<ImageProvider> provider_;
};

[[nodiscard]] Figure figured(Bytes raw, Seam<ImageProvider> images);

}
