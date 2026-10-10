#include "talcite/seam.hpp"

#include <string>

namespace talcite {

// A missing provider is an error on talcite's side, so it is reported in talcite's terms: as an
// exception, not as a string. One type for every missing provider: `catch (const Unbound&)` catches
// each of them, and catch (const runtime_error&) catches them together with everything else. A
// string could not be caught that way, and a promise rejected with a string would have a different
// type from every other refusal in the library.
Unbound::Unbound(std::string_view kind) : Trouble{"No provider for " + std::string{kind} + "."} {}

// A provider's refusal: the sentence from `decode`/`encode` turned into a talcite exception in one
// place. The kind's name comes first, so that what() says which seam refused before it says why.
Refused::Refused(std::string_view kind, std::string_view said)
    : Trouble{std::string{kind} + " provider refused: " + std::string{said}} {}

}
