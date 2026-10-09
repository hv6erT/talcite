#include "talcite/scan.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/child.hpp"

#ifdef _WIN32

#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>

#endif

// **Skan pliku programem antywirusowym systemu** — bez instalowania czegokolwiek przez nas.
//
// Na Windows pytamy usługę załączników (`IAttachmentExecute`): to ona woła zarejestrowany program
// antywirusowy — Defender albo inny — i przy okazji zapisuje znak pochodzenia pliku. Odmowa usługi
// znaczy, że plik został uznany za groźny (program antywirusowy mógł go już usunąć). Gdy usługi nie
// ma albo zawiedzie inaczej, zapasowo Defender z wiersza poleceń (`MpCmdRun -Scan`).
//
// Na Linuksie i innych POSIX — ClamAV, gdy jest: najpierw `clamdscan` (demon, szybko), potem
// `clamscan`. Kod wyjścia 0 to czysto, 1 to zagrożenie, reszta (brak bazy, brak demona) — nie wiadomo.
//
// Bez programu antywirusowego wynik to `unknown` z pustym `by`: wołający wie, że skanu nie było, i sam
// decyduje, co dalej. macOS sprawdza pliki sam przy uruchomieniu (XProtect), więc tu niczego nie woła.
namespace talcite::scan {

namespace {

std::string text_of(const Bytes& got) {
    std::string made;

    for (const std::byte one : got) made.push_back(static_cast<char>(one));

    while (!made.empty() && (made.back() == '\n' || made.back() == '\r')) made.pop_back();

    return made;
}

Promise<Scanned> ready(Scanned one) {
    return Promise<Scanned>{[one = std::move(one)](auto resolve, auto) mutable { resolve(std::move(one)); }};
}

// Program z wiersza poleceń, którego kod wyjścia mówi wynik; programu, którego nie ma, nie ma.
Promise<std::optional<Scanned>> asked(fs::path program, std::vector<std::string> arguments, int clean, int infected, std::string by) {
    Command what;

    what.program = std::move(program);
    what.arguments = std::move(arguments);
    what.merge = true;

    Promise<Output> running = Child::run(std::move(what));

    return Promise<void>::all_settled(running).then([running, clean, infected, by]() mutable -> std::optional<Scanned> {
        Output got;

        try {
            got = running.await();
        } catch (const std::exception&) {
            return std::nullopt;
        }

        if (got.exit.ended != Ended::normal) return Scanned{Verdict::unknown, by, text_of(got.out)};

#ifndef _WIN32
        // Program, którego nie dało się uruchomić (nie ma go), kończy dziecko kodem 127, jak w powłoce.
        if (got.exit.code == 127) return std::nullopt;
#endif

        if (got.exit.code == clean) return Scanned{Verdict::clean, by, text_of(got.out)};

        if (got.exit.code == infected) return Scanned{Verdict::infected, by, text_of(got.out)};

        return Scanned{Verdict::unknown, by, text_of(got.out)};
    });
}

#ifdef _WIN32

// Stałe usługi załączników podane wprost — bez biblioteki uuid, której MinGW i MSVC mają różne.
constexpr CLSID attachment_services{0x4125dd96, 0xe03a, 0x4103, {0x8f, 0x70, 0xe0, 0x59, 0x7d, 0x80, 0x3b, 0x9c}};
constexpr IID attachment_execute{0x73db1241, 0x1e85, 0x4581, {0x8e, 0x4f, 0xa8, 0x1e, 0x1d, 0x0f, 0x8c, 0x57}};

// Znak tej biblioteki dla usługi — usługa pamięta po nim ustawienia „pytaj / nie pytaj".
constexpr GUID talcite_client{0x6f1d3c2a, 0x8b4e, 0x4f61, {0x9a, 0x2d, 0x5c, 0x7e, 0x13, 0x40, 0xb8, 0x21}};

std::wstring wide(const std::string& one) {
    if (one.empty()) return {};

    const int much = MultiByteToWideChar(CP_UTF8, 0, one.data(), static_cast<int>(one.size()), nullptr, 0);

    std::wstring made(static_cast<std::size_t>(much), L'\0');

    MultiByteToWideChar(CP_UTF8, 0, one.data(), static_cast<int>(one.size()), made.data(), much);

    return made;
}

std::optional<Scanned> attached(const fs::path& where, const std::string& source) {
    const HRESULT began = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (FAILED(began) && began != RPC_E_CHANGED_MODE) return std::nullopt;

    std::optional<Scanned> made;
    IAttachmentExecute* service = nullptr;

    if (SUCCEEDED(CoCreateInstance(attachment_services, nullptr, CLSCTX_INPROC_SERVER, attachment_execute,
                                   reinterpret_cast<void**>(&service)))) {
        const std::wstring path = where.wstring();
        const std::wstring from = wide(source);

        if (SUCCEEDED(service->SetClientGuid(talcite_client)) && SUCCEEDED(service->SetLocalPath(path.c_str())) &&
            (from.empty() || SUCCEEDED(service->SetSource(from.c_str())))) {
            const HRESULT saved = service->Save();

            if (SUCCEEDED(saved)) made = Scanned{Verdict::clean, "attachment", {}};
            else if (saved == INET_E_SECURITY_PROBLEM || saved == E_FAIL || saved == HRESULT_FROM_WIN32(ERROR_VIRUS_INFECTED))
                made = Scanned{Verdict::infected, "attachment", std::to_string(static_cast<long>(saved))};
        }

        service->Release();
    }

    if (SUCCEEDED(began)) CoUninitialize();

    return made;
}

Promise<std::optional<Scanned>> defended(const fs::path& where) {
    std::vector<wchar_t> folder(MAX_PATH);

    const DWORD much = GetEnvironmentVariableW(L"ProgramFiles", folder.data(), static_cast<DWORD>(folder.size()));

    if (much == 0 || much >= folder.size())
        return Promise<std::optional<Scanned>>{[](auto resolve, auto) { resolve(std::optional<Scanned>{}); }};

    const fs::path program = fs::path{std::wstring{folder.data(), much}} / L"Windows Defender" / L"MpCmdRun.exe";

    return asked(program, {"-Scan", "-ScanType", "3", "-File", fs::to_string(where), "-DisableRemediation"}, 0, 2, "defender");
}

#endif

}

Promise<Scanned> scanned(fs::path where, std::string source) {
#ifdef _WIN32
    // Usługa załączników działa wprost, w wątku puli; zawiodła — Defender z wiersza poleceń.
    return Promise<std::optional<Scanned>>{[where, source = std::move(source)](auto resolve, auto) { resolve(attached(where, source)); }}
        .then([where](std::optional<Scanned> one) -> Promise<Scanned> {
            if (one) return ready(std::move(*one));

            return defended(where).then([](std::optional<Scanned> two) { return two.value_or(Scanned{}); });
        });
#elif defined(__APPLE__)
    static_cast<void>(where);
    static_cast<void>(source);

    return ready(Scanned{});
#else
    static_cast<void>(source);

    const std::string file = fs::to_string(where);

    return asked("clamdscan", {"--no-summary", "--infected", file}, 0, 1, "clamdscan")
        .then([file](std::optional<Scanned> one) -> Promise<Scanned> {
            if (one && one->verdict != Verdict::unknown) return ready(std::move(*one));

            return asked("clamscan", {"--no-summary", "--infected", file}, 0, 1, "clamscan").then([](std::optional<Scanned> two) {
                return two.value_or(Scanned{});
            });
        });
#endif
}

}
