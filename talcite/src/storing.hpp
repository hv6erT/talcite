#pragma once

#include <string>
#include <string_view>

namespace talcite::fs {

// Postać przechowywana ścieżki (`stored`, `restored`) osobno dla obu rodzajów nazw systemowych:
// bajtów POSIX-a i jednostek UTF-16 Windows. Obie gałęzie są zwykłymi funkcjami, więc próby
// sprawdzają je na każdym systemie, a nie tylko na tym, którego nazwy akurat są natywne.
[[nodiscard]] std::string stored_bytes(std::string_view native);

[[nodiscard]] std::string stored_units(std::u16string_view native);

[[nodiscard]] std::string restored_bytes(std::string_view said);

[[nodiscard]] std::u16string restored_units(std::string_view said);

}
