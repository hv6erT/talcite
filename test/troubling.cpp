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

// **Jeden korzeń dla wszystkiego, co może się nie udać w biegu.** Kto chce pokazać okno błędu,
// łapie `Trouble` i nie łapie przy tym cudzych wyjątków ani złego użycia (`std::logic_error`).
TRIAL("Trouble: błędy biblioteki mają wspólny korzeń") {
    ought((std::is_base_of_v<Trouble, fs::Trouble>));
    ought((std::is_base_of_v<Trouble, net::Trouble>));
    ought((std::is_base_of_v<Trouble, Rejected>));
    ought((std::is_base_of_v<Trouble, Overdue>));
    ought((std::is_base_of_v<Trouble, Unbound>));
    ought((std::is_base_of_v<Trouble, Refused>));
}

// **Przerwanie nie jest awarią.** Kto przerwał, ten wie; okno błędu o anulowaniu byłoby kłamstwem,
// a twa odsiewało je dotąd ręcznie, po typie. Dlatego `Cancelled` stoi obok korzenia, nie pod nim.
TRIAL("Trouble: anulowanie stoi poza korzeniem") {
    ought((!std::is_base_of_v<Trouble, Cancelled>));
    ought((std::is_base_of_v<std::runtime_error, Cancelled>));
}

// Kod systemowy niesie korzeń, więc gałęzie go nie powtarzają: kłopot sieciowy oddaje ten sam
// kod, który dostał, a zdanie zostaje zdaniem wołającego.
TRIAL("Trouble: kod systemowy siedzi w korzeniu") {
    const net::Trouble one{"Nie wyszło.", 5};

    const Trouble& root = one;

    same(root.code(), 5);
    same(std::string{root.what()}, std::string{"Nie wyszło."});
    same(Trouble{"Bez kodu."}.code(), 0);
}

// Odrzucenie napisem, obietnica po terminie i odmowa szwu wracają jako `Trouble` — to są te
// ścieżki, którymi błąd dochodzi do aplikacji w praktyce.
TRIAL("Trouble: odrzucenie napisem łapie się jako Trouble") {
    Promise<int> one{[](auto, auto reject) { reject("Nie ma."); }};

    bool caught = false;

    try {
        (void)one.await();
    } catch (const Trouble& why) {
        caught = std::string{why.what()} == "Nie ma.";
    }

    ought(caught);
}
