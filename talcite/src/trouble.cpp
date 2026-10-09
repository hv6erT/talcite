#include "talcite/trouble.hpp"

namespace talcite {

// **Wspólny korzeń błędów czasu biegu** (przegląd, krok 6a). Dawniej `fs::Trouble`,
// `net::Trouble`, `Overdue`, `Unbound`, `Refused` i `Rejected` stały każdy osobno na
// `std::runtime_error`, więc „cokolwiek z talcite się nie udało" dało się złapać tylko razem
// z cudzymi wyjątkami. Teraz wszystkie idą stąd; `std::logic_error` zostaje na złe użycie,
// a `Cancelled` stoi obok, bo przerwanie nie jest awarią.
//
// Kod systemowy jest tu, a nie w jednej z gałęzi: errno przychodzi i z plików, i z sieci,
// i z jądra liczącego skróty, a dawniej miał go tylko kłopot sieciowy — więc błąd skrótu
// udawał sieciowy, żeby móc go nieść.
Trouble::Trouble(const std::string& said, int code) : std::runtime_error(said), code_(code) {}

}
