#include "talcite/seam.hpp"

#include <string>

namespace talcite {

// Brak dostawcy to błąd po stronie talcite, więc idzie językiem talcite — wyjątkiem, nie napisem.
// Jeden typ na każdy brak: `catch (const Unbound&)` łapie każdy, a `catch (const runtime_error&)`
// razem z resztą. Napis nie dałby się złapać, a odrzucenie obietnicy zrobione z napisu wychodziło
// kiedyś jako inny typ niż wszystkie pozostałe odmowy w bibliotece — lekcja z `secured.cpp`.
Unbound::Unbound(std::string_view kind) : Trouble{"No provider for " + std::string{kind} + "."} {}

// Odmowa dostawcy: zdanie z `decode`/`encode` przetłumaczone na wyjątek talcite w jednym miejscu.
// Nazwa rodzaju z przodu, żeby `what()` mówił, który szew odmówił, zanim powie dlaczego.
Refused::Refused(std::string_view kind, std::string_view said)
    : Trouble{std::string{kind} + " provider refused: " + std::string{said}} {}

}
