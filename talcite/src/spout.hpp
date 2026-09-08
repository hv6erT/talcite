#pragma once

#include <string_view>

namespace talcite {

// Bajty prosto na wyjście, z pominięciem wszystkiego. Stoi tu, bo potrzebują tego trzy pliki —
// obraz, badanie terminala i schowek — a trzy kopie tej samej pętli zapisu prędzej czy później
// zaczęłyby się różnić.
//
// **Nie ma tu buforowania i nie ma obsługi błędu.** Krótki zapis dobieramy w pętli, a zapis, który
// się nie udał, milknie: nie ma komu tego zgłosić, bo na drugim końcu jest terminal, a nie plik.
void writing(std::string_view bytes);

}
