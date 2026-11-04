#pragma once

#include <chrono>
#include <exception>

#include "talcite/address.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"

// The small pieces every networking file of talcite needs alike, each written once.

namespace talcite::net {

// The time left until `until`, rounded up to a whole millisecond and at least one, so that it can
// be passed as a patience and a late call still gets a nonzero one. Rounding down would let a wait
// end before `until`, and the caller, seeing time left, would start another round in the last
// fraction of a millisecond.
[[nodiscard]] inline Patience left(std::chrono::steady_clock::time_point until) {
    const auto rest = std::chrono::ceil<Patience>(until - std::chrono::steady_clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

// A promise already rejected with `Trouble` and the given text.
template <typename T>
[[nodiscard]] Promise<T> refused(const char* said) {
    return failed<T>(std::make_exception_ptr(Trouble{said}));
}

// The empty address a closed stream, channel, socket or beacon reports. Every one of them answers
// politely after `close()`, since reaching for a state that is gone would be a memory violation,
// and the class is meant to stay usable after closing.
[[nodiscard]] inline const Address& nowhere() {
    static const Address one;

    return one;
}

}
