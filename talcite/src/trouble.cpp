#include "talcite/trouble.hpp"

namespace talcite {

// The common root of runtime errors. `fs::Trouble`, `net::Trouble`, `Overdue`, `Unbound`,
// `Refused` and `Rejected` all derive from it, so "anything in talcite failed" can be caught
// without catching other libraries' exceptions too. `std::logic_error` stays for misuse, and
// `Cancelled` stands beside it, because cancellation is not a failure.
//
// The system code lives here and not in one of the branches: errno comes from files, from the
// network and from the kernel computing hashes alike.
Trouble::Trouble(const std::string& said, int code) : std::runtime_error(said), code_(code) {}

}
