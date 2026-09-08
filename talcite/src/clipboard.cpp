#include "talcite/clipboard.hpp"

#include <cstddef>
#include <cstdlib>

#include "spout.hpp"

#ifdef _WIN32

#include <windows.h>

#endif

namespace talcite {

namespace {

// Ostatnia własna kopia. Na POSIX-ie to jest **cała** odpowiedź `pasted`, bo terminala o schowek
// nie pytamy — jego odpowiedź wsypałaby się do programu jako klawisze, dopóki dekoder nie zna OSC.
// Na Windows to zapas na wypadek, gdyby schowek systemu odmówił otwarcia.
//
// To nie jest własny schowek udający systemowy: to jedna zmienna po to, żeby wytnij i wklej
// **w tym samym programie** działało wszędzie. Wklejanie z zewnątrz robi użytkownik klawiszem
// terminala i przychodzi jako `PasteEvent`, więc tamtej drogi to nie dotyczy.
std::string mine;

// Ile bajtów treści wolno wysłać w OSC 52. Terminale ucinają długie ciągi **po cichu**, a próg
// mają różny i żaden go nie ogłasza, więc powyżej naszego nie wysyłamy nic: lepiej „nie poszło"
// niż ogryzek udający całość. W programie kopia zostaje pełna.
constexpr std::size_t brim = 64 * 1024;

constexpr const char* letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Base64 z dopełnieniem, bo takiego oczekują terminale. Dwadzieścia linijek zamiast zależności —
// to nie jest kryptografia, tylko przepisanie trzech bajtów na cztery znaki.
std::string sixed(std::string_view what) {
    std::string made;

    made.reserve((what.size() + 2) / 3 * 4);

    std::size_t at = 0;

    while (at + 2 < what.size()) {
        const auto one = static_cast<unsigned char>(what[at]);
        const auto two = static_cast<unsigned char>(what[at + 1]);
        const auto three = static_cast<unsigned char>(what[at + 2]);

        made.push_back(letters[one >> 2]);
        made.push_back(letters[((one & 0x03) << 4) | (two >> 4)]);
        made.push_back(letters[((two & 0x0F) << 2) | (three >> 6)]);
        made.push_back(letters[three & 0x3F]);

        at += 3;
    }

    if (at + 1 == what.size()) {
        const auto one = static_cast<unsigned char>(what[at]);

        made.push_back(letters[one >> 2]);
        made.push_back(letters[(one & 0x03) << 4]);
        made.push_back('=');
        made.push_back('=');
    } else if (at + 2 == what.size()) {
        const auto one = static_cast<unsigned char>(what[at]);
        const auto two = static_cast<unsigned char>(what[at + 1]);

        made.push_back(letters[one >> 2]);
        made.push_back(letters[((one & 0x03) << 4) | (two >> 4)]);
        made.push_back(letters[(two & 0x0F) << 2]);
        made.push_back('=');
    }

    return made;
}

// Prośba do terminala, żeby wstawił to do schowka **swojej** maszyny. `c` to schowek systemowy;
// wyboru pierwotnego (`p`, środkowy przycisk myszy) nie ruszamy — jeden schowek, jedna treść.
//
// Wewnątrz tmuksa przechodzi to tylko przy `set-clipboard on`. Świadomie **nie opakowujemy** tego
// w przekazanie tmuksowe: na nowych wersjach opakowanie psuje to, co bez niego działa, a wersji
// tmuksa nie da się stąd poznać inaczej niż zgadywaniem.
bool sent(std::string_view what) {
    if (what.size() > brim) return false;

    writing("\x1b]52;c;" + sixed(what) + "\x07");

    return true;
}

// Przez ssh schowek tego procesu jest schowkiem **nie tej maszyny**, przy której siedzi
// użytkownik. Wtedy jedyną poprawną drogą jest poprosić terminal.
bool remote() {
    return std::getenv("SSH_CONNECTION") != nullptr || std::getenv("SSH_TTY") != nullptr;
}

#ifdef _WIN32

// Schowek Windows trzyma wiersze rozdzielone parą, a dokument pojedynczym znakiem. Bez zamiany
// trzy skopiowane wiersze byłyby w Notatniku jednym.
std::wstring widened(std::string_view what) {
    std::string full;

    full.reserve(what.size() + what.size() / 16);

    for (const char one : what) {
        if (one == '\n') full.push_back('\r');

        full.push_back(one);
    }

    if (full.empty()) return {};

    const int much = MultiByteToWideChar(CP_UTF8, 0, full.data(), static_cast<int>(full.size()),
                                         nullptr, 0);

    if (much <= 0) return {};

    std::wstring made(static_cast<std::size_t>(much), L'\0');

    MultiByteToWideChar(CP_UTF8, 0, full.data(), static_cast<int>(full.size()), made.data(), much);

    return made;
}

std::string narrowed(const wchar_t* what) {
    if (what == nullptr) return {};

    const int much = WideCharToMultiByte(CP_UTF8, 0, what, -1, nullptr, 0, nullptr, nullptr);

    if (much <= 1) return {};

    std::string full(static_cast<std::size_t>(much - 1), '\0');

    WideCharToMultiByte(CP_UTF8, 0, what, -1, full.data(), much - 1, nullptr, nullptr);

    std::string made;

    made.reserve(full.size());

    for (std::size_t at = 0; at < full.size(); ++at) {
        if (full[at] == '\r' && at + 1 < full.size() && full[at + 1] == '\n') continue;

        made.push_back(full[at]);
    }

    return made;
}

// Otwarcie potrafi się nie udać, bo schowek trzyma w tej chwili inny proces. Kilka natychmiastowych
// ponowień wystarcza; usypianie odpada, bo zamroziłoby pętlę zdarzeń.
bool opened() {
    for (int which = 0; which < 8; ++which)
        if (OpenClipboard(nullptr) != 0) return true;

    return false;
}

#endif

}

#ifdef _WIN32

bool copied(std::string_view what) {
    mine.assign(what);

    if (remote()) return sent(what);

    const std::wstring wide = widened(what);

    if (!opened()) return false;

    // Od tego miejsca **każda** droga wyjścia musi zamknąć schowek: zostawiony otwarty blokuje
    // schowek całego pulpitu, nie tylko nasz.
    if (EmptyClipboard() == 0) {
        CloseClipboard();

        return false;
    }

    const std::size_t much = (wide.size() + 1) * sizeof(wchar_t);

    const HGLOBAL room = GlobalAlloc(GMEM_MOVEABLE, much);

    if (room == nullptr) {
        CloseClipboard();

        return false;
    }

    auto* said = static_cast<wchar_t*>(GlobalLock(room));

    if (said == nullptr) {
        GlobalFree(room);
        CloseClipboard();

        return false;
    }

    for (std::size_t at = 0; at < wide.size(); ++at) said[at] = wide[at];

    said[wide.size()] = L'\0';

    GlobalUnlock(room);

    // **Po udanym oddaniu pamięć należy do systemu** i zwolnienie jej byłoby uszkodzeniem schowka.
    // Po nieudanym należy dalej do nas i to my musimy ją zwolnić.
    const bool went = SetClipboardData(CF_UNICODETEXT, room) != nullptr;

    if (!went) GlobalFree(room);

    CloseClipboard();

    return went;
}

std::optional<std::string> pasted() {
    if (remote()) return mine.empty() ? std::optional<std::string>{} : mine;

    if (!opened()) return mine.empty() ? std::optional<std::string>{} : mine;

    const HANDLE room = GetClipboardData(CF_UNICODETEXT);

    if (room == nullptr) {
        CloseClipboard();

        return {};
    }

    const auto* said = static_cast<const wchar_t*>(GlobalLock(room));

    std::string made = narrowed(said);

    if (said != nullptr) GlobalUnlock(room);

    CloseClipboard();

    if (made.empty()) return {};

    return made;
}

#else

bool copied(std::string_view what) {
    mine.assign(what);

    return sent(what);
}

std::optional<std::string> pasted() {
    if (mine.empty()) return {};

    return mine;
}

#endif

}
