#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "proving.hpp"

// Spójność CLAUDE.md z kodem: każda nazwa ujęta w grzbietach w odwrotne apostrofy musi istnieć
// w źródłach. Grzbiety, które mówią o rzeczach nieistniejących, są gorsze niż brak grzbietów —
// bo wyglądają na prawdę.

namespace {

namespace stood = std::filesystem;

// Nazwy obce (OpenSSL, Win32, POSIX, klucze skórek MC) i świadomie odrzucone pomysły są tu
// wypisane wprost, żeby wynik dało się czytać — wszystko poza tą listą jest podejrzane.
const std::set<std::string> foreign = {
    // OpenSSL, Win32, POSIX, biblioteka standardowa i narzędzia
    "BCryptGenRandom", "BIO_read", "BIO_s_mem", "BIO_write", "CreatePermission", "EVP_RSA_gen",
    "FindFirstFileW", "INT_MAX", "TALCITE_OPENSSL", "NOMINMAX", "SSL", "SSL_CTX", "SSL_new",
    "SSL_read", "SSL_set_bio", "SSL_write", "arc4random_buf", "libSystem", "mt19937_64", "wcwidth",
    "ws2_32", "mpr", "wtools", "conditional_t", "std::conditional_t", "compare_exchange", "iocp",
    "pty", "device_bash", "XDG_RUNTIME_DIR", "socketpair", "DeleteFile", "CMakeLists",
    "FINGERPRINT", "NONCE", "REALM", "USERNAME", "ONESHOT", "_sync", "_default_",
    // nazwy z Midnight Commandera i jego skórek
    "buttonbar", "dfocus", "dhotfocus", "dhotnormal", "dtitle", "hline", "hotkey", "HARDLINK",
    "input_complete", "query_dialog", "radio", "groupbox", "group", "shadow", "color", "check",
    "Colour",
    // pomysły opisane jako odrzucone albo historyczne
    "NetPromise", "Usher", "Reactor", "Halt", "LegendStyle", "Sealing", "Setting", "Body", "deed",
    "shut", "deleteLater", "preventDefault", "allSettled", "eager", "halted", "roam",
    "Dialog::roam", "Dialog::based", "Dialog::hotted", "talcite::gui", "talcite::tui",
    "showing::Speaking", "read_byte", "get_def",
    // rdzenie nazw zdarzeń, wymieniane bez przyrostka
    "Click", "Close", "DoubleClick", "Focus", "Hot", "Paste", "Pick", "Unfocus", "Wheel",
    "Handlery", "Digest",
};

// Korzeń projektu liczony od tego pliku: `test/naming.cpp` leży o jeden katalog niżej.
stood::path root() { return stood::path{__FILE__}.parent_path().parent_path(); }

std::string slurped(const stood::path& where) {
    std::ifstream input{where, std::ios::binary};

    std::ostringstream held;

    held << input.rdbuf();

    return held.str();
}

std::string sources() {
    std::string whole;

    for (const char* base : {"talcite", "test", "demo"}) {
        for (const auto& one : stood::recursive_directory_iterator{root() / base}) {
            if (!one.is_regular_file()) continue;

            const std::string ending = one.path().extension().string();

            if (ending == ".hpp" || ending == ".cpp") whole += slurped(one.path());
        }
    }

    return whole;
}

bool wordy(char one) { return std::isalnum(static_cast<unsigned char>(one)) != 0 || one == '_'; }

// Wystąpienie całym słowem, a nie kawałkiem innego: `put` nie może zaliczyć się przez `output`.
bool present(std::string_view whole, std::string_view stem) {
    for (std::size_t at = whole.find(stem); at != std::string_view::npos;
         at = whole.find(stem, at + 1)) {
        const bool before = at == 0 || !wordy(whole[at - 1]);
        const bool after = at + stem.size() == whole.size() || !wordy(whole[at + stem.size()]);

        if (before && after) return true;
    }

    return false;
}

}

TRIAL("Nazwy: każda nazwa z grzbietów istnieje w kodzie") {
    const std::string doc = slurped(root() / "CLAUDE.md");

    ought(!doc.empty());

    const std::regex backticked{R"(`([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)?)\(?\)?`)"};

    std::set<std::string> names;

    for (auto it = std::sregex_iterator{doc.begin(), doc.end(), backticked};
         it != std::sregex_iterator{}; ++it)
        names.insert((*it)[1].str());

    const std::string whole = sources();

    std::vector<std::string> bad;

    for (const std::string& one : names) {
        if (foreign.contains(one)) continue;

        const std::size_t colon = one.rfind("::");
        const std::string stem = colon == std::string::npos ? one : one.substr(colon + 2);

        if (stem.size() < 3) continue;

        if (!present(whole, stem)) bad.push_back(one);
    }

    if (!bad.empty()) {
        std::string said = "nazwy z CLAUDE.md, których nie ma w kodzie:";

        for (const std::string& one : bad) said += "\n        " + one;

        ::proving::fell(__FILE__, __LINE__, said);
    }
}
