#pragma once

#include <cstdint>
#include <string>

#include "talcite/fs.hpp"
#include "talcite/promise.hpp"

namespace talcite::scan {

enum class Verdict : std::uint8_t { clean, infected, unknown };

struct Scanned {
    Verdict verdict = Verdict::unknown;
    std::string by;
    std::string said;
};

[[nodiscard]] Promise<Scanned> scanned(fs::path where, std::string source = {});

}
