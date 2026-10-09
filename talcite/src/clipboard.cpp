#include "talcite/clipboard.hpp"

#include "talcite/encoding.hpp"

#include <cstddef>
#include <memory>
#include <utility>

#ifdef _WIN32

#include <windows.h>

#else

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

#include <unistd.h>

#include "talcite/child.hpp"
#include "talcite/time.hpp"

#endif

namespace talcite {

namespace {

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
bool sent(Console& out, std::string_view what) {
    if (what.size() > brim) return false;

    out.write("\x1b]52;c;" + sixed(what) + "\x07");

    return true;
}

// Przez ssh schowek tego procesu jest schowkiem **nie tej maszyny**, przy której siedzi
// użytkownik. Wtedy jedyną poprawną drogą jest poprosić terminal. Środowisko czytamy z konsoli,
// bo to ona wie, gdzie siedzi użytkownik — i dzięki temu próba może udawać ssh na każdym systemie.
bool remote(const Console& from) {
    return from.variable("SSH_CONNECTION").has_value() || from.variable("SSH_TTY").has_value();
}

#ifndef _WIN32

// Czy program jest w PATH — tak samo, jak szuka go powłoka.
bool present(const std::string& named) {
    const char* whole = std::getenv("PATH");

    if (whole == nullptr) return false;

    const std::string said{whole};

    for (std::size_t at = 0; at <= said.size();) {
        const std::size_t next = std::min(said.find(':', at), said.size());

        const std::string one = said.substr(at, next - at);

        if (!one.empty() && ::access((one + "/" + named).c_str(), X_OK) == 0) return true;

        at = next + 1;
    }

    return false;
}

// **Schowek sesji graficznej przez jej własny program**: `wl-copy` pod Waylandem, `xclip` albo
// `xsel` pod X, `pbcopy` na macOS. OSC 52 zostaje zapasem: GNOME Terminal i reszta rodziny VTE go
// nie znają, więc kopia z twa nie docierała tam do schowka wcale. Przez ssh tej drogi nie ma —
// program schowka tamtej maszyny to nie schowek użytkownika.
//
// Każdy z nich zostawia po sobie proces pilnujący schowka; przez powłokę z wyjściem na /dev/null,
// żeby ten proces nie trzymał naszych rur i nie wstrzymał końca.
std::optional<Command> helped(const Console& from) {
    std::string line;

    if (from.variable("WAYLAND_DISPLAY").has_value() && present("wl-copy")) line = "exec wl-copy";
    else if (from.variable("DISPLAY").has_value() && present("xclip")) line = "exec xclip -selection clipboard -in";
    else if (from.variable("DISPLAY").has_value() && present("xsel")) line = "exec xsel --clipboard --input";
#ifdef __APPLE__
    else if (present("pbcopy")) line = "exec pbcopy";
#endif

    if (line.empty()) return std::nullopt;

    Command made;

    made.program = "/bin/sh";
    made.arguments = {"-c", line + " >/dev/null 2>&1"};

    return made;
}

// Oddanie treści programowi schowka; prawda, gdy skończył bez błędu. Czekamy najwyżej półtorej
// sekundy — program, który stanął, ustępuje miejsca OSC 52, zamiast zamrozić pętlę.
bool handed(const Command& what, std::string_view said) {
    try {
        const Child one = Child::start(what).await();

        Promise<std::size_t> writing = one.write(bytes(std::string{said}));

        (void)writing.await();

        one.close_input();

        Promise<Exit> ending = one.wait();
        Promise<void> limit = Time::limit(std::chrono::milliseconds{1500});

        try {
            Promise<void>::race(ending, limit).await();
        } catch (...) {
            one.kill();

            return false;
        }

        limit.cancel();

        const Exit made = ending.await();

        return made.ended == Ended::normal && made.code == 0;
    } catch (...) {
        return false;
    }
}

#endif

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

    return to_wstring(full);
}

std::string narrowed(const wchar_t* what) {
    if (what == nullptr) return {};

    const std::string full = to_string(std::wstring_view{what});

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

#ifdef _WIN32

// Oddanie tekstu schowkowi systemu Windows.
bool handed(std::string_view what) {
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

// Brak otwarcia to co innego niż pusty schowek: wtedy wołający sięga po własną kopię.
std::optional<std::optional<std::string>> taken() {
    if (!opened()) return std::nullopt;

    const HANDLE room = GetClipboardData(CF_UNICODETEXT);

    if (room == nullptr) {
        CloseClipboard();

        return std::optional<std::string>{};
    }

    const auto* said = static_cast<const wchar_t*>(GlobalLock(room));

    std::string made = narrowed(said);

    if (said != nullptr) GlobalUnlock(room);

    CloseClipboard();

    if (made.empty()) return std::optional<std::string>{};

    return std::optional<std::string>{std::move(made)};
}

#endif

// Schowek maszyny, przy której siedzi użytkownik. Na Windows na miejscu systemowy, a przez ssh
// terminal; na POSIX-ie zawsze terminal.
class System final : public Clipboard {
public:
    explicit System(Seam<Console> with) : console_(std::move(with)) {}

    [[nodiscard]] std::string_view name() const override { return "system clipboard"; }

    [[nodiscard]] bool copy(std::string_view what) override {
        mine_.assign(what);

        Console& out = console_.provider();

#ifdef _WIN32

        if (!remote(out)) return handed(what);

#else

        if (!remote(out))
            if (const std::optional<Command> helper = helped(out); helper && handed(*helper, what)) return true;

#endif

        return sent(out, what);
    }

    [[nodiscard]] std::optional<std::string> paste() override {
#ifdef _WIN32

        if (!remote(console_.provider()))
            if (std::optional<std::optional<std::string>> got = taken(); got.has_value())
                return std::move(*got);

#endif

        if (mine_.empty()) return std::nullopt;

        return mine_;
    }

private:
    Seam<Console> console_;

    // Ostatnia własna kopia. Na POSIX-ie to jest **cała** odpowiedź `paste`, bo terminala o schowek
    // nie pytamy — jego odpowiedź wsypałaby się do programu jako klawisze, dopóki dekoder nie zna
    // OSC. Na Windows to zapas na wypadek, gdyby schowek systemu odmówił otwarcia.
    //
    // To nie jest własny schowek udający systemowy: to jedna zmienna po to, żeby wytnij i wklej
    // **w tym samym programie** działało wszędzie. Wklejanie z zewnątrz robi użytkownik klawiszem
    // terminala i przychodzi jako `PasteEvent`, więc tamtej drogi to nie dotyczy.
    std::string mine_;
};

}

// **Jeden na proces**, bo własna kopia ma być wspólna: wycięte w jednym edytorze wkleja się
// w drugim. Kto chce osobnego — próba, drugi terminal — bierze wersję z konsolą.
Seam<Clipboard> clipboard() {
    static const Seam<Clipboard> shared{std::make_shared<System>(console())};

    return shared;
}

Seam<Clipboard> clipboard(Seam<Console> with) {
    return Seam<Clipboard>{std::make_shared<System>(std::move(with))};
}

}
