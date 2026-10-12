#include "proving.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>

#include "talcite/address.hpp"
#include "talcite/fs.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"
#include "talcite/time.hpp"
#include "talcite/trouble.hpp"

using namespace talcite;

// One root for everything that can fail at run time. Whoever wants to show an error window catches
// `Trouble`, and does not catch foreign exceptions or misuse (`std::logic_error`) along with it.
TRIAL("Trouble: the library's errors share one root") {
    ought((std::is_base_of_v<Trouble, fs::Trouble>));
    ought((std::is_base_of_v<Trouble, net::Trouble>));
    ought((std::is_base_of_v<Trouble, Rejected>));
    ought((std::is_base_of_v<Trouble, Overdue>));
    ought((std::is_base_of_v<Trouble, Unbound>));
    ought((std::is_base_of_v<Trouble, Refused>));
}

// An interruption is not a failure. Whoever interrupted knows it; an error window about a
// cancellation would be a lie. That is why `Cancelled` stands beside the root, not under it.
TRIAL("Trouble: cancellation stands outside the root") {
    ought((!std::is_base_of_v<Trouble, Cancelled>));
    ought((std::is_base_of_v<std::runtime_error, Cancelled>));
}

// The root carries the system code, so the branches do not repeat it: a network trouble gives back
// the same code it got, and the sentence stays the caller's sentence.
TRIAL("Trouble: the system code sits in the root") {
    const net::Trouble one{"Nie wyszło.", 5};

    const Trouble& root = one;

    same(root.code(), 5);
    same(std::string{root.what()}, std::string{"Nie wyszło."});
    same(Trouble{"Bez kodu."}.code(), 0);
}

// A rejection with a string, a promise past its deadline and a seam's refusal come back as
// `Trouble`; these are the paths by which an error reaches the application in practice.
TRIAL("Trouble: a rejection with a string is caught as Trouble") {
    auto [one, settle] = promised<int>();

    (void)settle.fail("Nie ma.");

    bool caught = false;

    try {
        (void)std::move(one).get();
    } catch (const Trouble& why) {
        caught = std::string{why.what()} == "Nie ma.";
    }

    ought(caught);
}
