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

// Scanning a file with the system's antivirus program, without installing anything ourselves.
//
// On Windows we ask the attachment service (`IAttachmentExecute`): it calls the registered
// antivirus program (Defender or another one) and also records the file's mark of origin. A refusal
// from the service means the file was judged dangerous (the antivirus may already have removed it).
// When the service is missing or fails in some other way, the fallback is Defender's command line
// (MpCmdRun -Scan).
//
// On Linux and other POSIX systems we use ClamAV when present: first clamdscan (the daemon, fast),
// then clamscan. Exit code 0 means clean, 1 means a threat, anything else (no database, no daemon)
// means unknown.
//
// Without an antivirus program the result is `unknown` with an empty `by`: the caller knows no scan
// took place and decides what to do next. macOS checks files itself when they are launched
// (XProtect), so nothing is called there.
namespace talcite::scan {

namespace {

// The program's output as text, with trailing line breaks removed.
std::string text_of(const Bytes& got) {
    std::string made;

    for (const std::byte one : got) made.push_back(static_cast<char>(one));

    while (!made.empty() && (made.back() == '\n' || made.back() == '\r')) made.pop_back();

    return made;
}

// Runs a command-line scanner whose exit code gives the verdict (`clean` and `infected` are the
// codes for the two definite answers). A program that cannot be started yields `std::nullopt`.
Promise<std::optional<Scanned>> asked(fs::path program, std::vector<std::string> arguments, int clean, int infected, std::string by) {
    Command what;

    what.program = std::move(program);
    what.arguments = std::move(arguments);
    what.merge = true;

    return Child::run(std::move(what)).outcome().then([clean, infected, by](Outcome<Output> ran) -> std::optional<Scanned> {
        if (!ran.ok()) return std::nullopt;

        const Output got = std::move(ran).value();

        if (got.exit.ended != Ended::normal) return Scanned{Verdict::unknown, by, text_of(got.out)};

        if (got.exit.code == clean) return Scanned{Verdict::clean, by, text_of(got.out)};

        if (got.exit.code == infected) return Scanned{Verdict::infected, by, text_of(got.out)};

        return Scanned{Verdict::unknown, by, text_of(got.out)};
    });
}

#ifdef _WIN32

// The attachment service's identifiers spelled out here, so we do not need the uuid library, which
// differs between MinGW and MSVC.
constexpr CLSID attachment_services{0x4125dd96, 0xe03a, 0x4103, {0x8f, 0x70, 0xe0, 0x59, 0x7d, 0x80, 0x3b, 0x9c}};
constexpr IID attachment_execute{0x73db1241, 0x1e85, 0x4581, {0x8e, 0x4f, 0xa8, 0x1e, 0x1d, 0x0f, 0x8c, 0x57}};

// This library's identity for the service; the service keys its "ask / don't ask" settings by it.
constexpr GUID talcite_client{0x6f1d3c2a, 0x8b4e, 0x4f61, {0x9a, 0x2d, 0x5c, 0x7e, 0x13, 0x40, 0xb8, 0x21}};

// Converts UTF-8 text to UTF-16 for the Windows API.
std::wstring wide(const std::string& one) {
    if (one.empty()) return {};

    const int much = MultiByteToWideChar(CP_UTF8, 0, one.data(), static_cast<int>(one.size()), nullptr, 0);

    std::wstring made(static_cast<std::size_t>(much), L'\0');

    MultiByteToWideChar(CP_UTF8, 0, one.data(), static_cast<int>(one.size()), made.data(), much);

    return made;
}

// Asks the attachment service to check the file (`source` is where it came from, possibly empty).
// Success means clean; a security refusal means infected; `std::nullopt` when COM or the service is
// unavailable or fails in some other way.
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

// Scans the file with Defender's MpCmdRun.exe from %ProgramFiles% (exit 0 is clean, 2 is a threat);
// `std::nullopt` when the folder cannot be found or the program does not run.
Promise<std::optional<Scanned>> defended(const fs::path& where) {
    std::vector<wchar_t> folder(MAX_PATH);

    const DWORD much = GetEnvironmentVariableW(L"ProgramFiles", folder.data(), static_cast<DWORD>(folder.size()));

    if (much == 0 || much >= folder.size())
        return run(io(), []() -> std::optional<Scanned> { return std::optional<Scanned>{}; });

    const fs::path program = fs::path{std::wstring{folder.data(), much}} / L"Windows Defender" / L"MpCmdRun.exe";

    return asked(program, {"-Scan", "-ScanType", "3", "-File", fs::to_string(where), "-DisableRemediation"}, 0, 2, "defender");
}

#endif

}

// Scans one file with whatever antivirus the system offers (see the top of this file); the result
// is `unknown` with an empty `by` when there is none.
Promise<Scanned> scanned(fs::path where, std::string source) {
#ifdef _WIN32
    // The attachment service is called directly, on a pool thread; if it fails, Defender's command
    // line is tried.
    return run(io(), [where, source = std::move(source)]() -> std::optional<Scanned> { return attached(where, source); })
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
