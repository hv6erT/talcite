#include "talcite/random.hpp"

#include <cstdint>
#include <exception>
#include <random>
#include <string>

#include "talcite/trouble.hpp"

namespace talcite {

// Bierzemy `std::random_device`, a nie własny generator i nie wywołanie systemowe wprost. To jest
// jedyne miejsce w standardzie, które **ma być** źródłem entropii, a nie ciągiem wyliczanym
// z ziarna: na MSVC czyta `RtlGenRandom`, na glibc `/dev/urandom` albo `RDSEED`. Jedna droga na
// wszystkie systemy, bez linkowania czegokolwiek i bez ścieżek pisanych w ciemno.
//
// **Generatora pseudolosowego tu nie ma i nie może być.** `mt19937` powstał do symulacji: jego
// stan odtwarza się z trzystu dwunastu kolejnych wyników, bo mieszanie jest odwracalne. Kto zobaczy
// tyle liczb, zna wszystkie następne — a numer transakcji STUN jest **jedynym dowodem tożsamości
// odpowiedzi**, więc przewidywalny otwiera przejęcie rozmowy.
//
// Czego standard **nie obiecuje**: że `random_device` naprawdę jest losowy. Wolno mu być zwykłym
// generatorem deterministycznym, a znany przypadek — MinGW przez lata oddawał ten sam ciąg przy
// każdym uruchomieniu — pokazuje, że to nie jest teoretyczne. Sprawdzić tego z wnętrza programu
// się nie da: `entropy()` bywa zerowe także tam, gdzie źródło jest prawdziwe.
Bytes random(std::size_t much) {
    Bytes made(much);

    if (much == 0) return made;

    // Urządzenie zakładamy **raz na wątek**: jego utworzenie potrafi otworzyć plik albo sięgnąć do
    // sprzętu, a robienie tego przy każdym wywołaniu kosztowałoby więcej niż samo losowanie.
    static thread_local std::random_device rolling;

    try {
        std::size_t at = 0;

        while (at < made.size()) {
            const auto one = static_cast<std::uint_fast64_t>(rolling());

            for (int which = 0; which < 4 && at < made.size(); ++which, ++at)
                made[at] = static_cast<std::byte>((one >> (8 * which)) & 0xFF);
        }
    } catch (const std::exception& why) {
        throw net::Trouble{std::string{"System nie dał losowości: "} + why.what() + "."};
    }

    return made;
}

}
