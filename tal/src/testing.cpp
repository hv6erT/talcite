#include "tal/testing.hpp"

#include "tal/log.hpp"

namespace tal::testing {

// While it lives, an unhandled failure goes to the log only and does not stop a debug build.
Expected::Expected() : start_(detail::unhandled_count()) { detail::tolerate(1); }

// Unhandled failures stop a debug build again, unless another guard still expects them.
Expected::~Expected() { detail::tolerate(-1); }

// How many unhandled failures came, from any thread, since the guard was made.
std::uint64_t Expected::failures() const { return detail::unhandled_count() - start_; }

// A guard for a trial that makes work fail unhandled on purpose.
Expected expect_failure() { return {}; }

}
