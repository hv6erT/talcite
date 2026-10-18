#include "talcite/launcher.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include "talcite/encoding.hpp"
#include "talcite/place.hpp"

#else

#include "talcite/child.hpp"

#endif

#include "remoting.hpp"

// Opening an address in the program the user's system gives it: the browser for a web page, the
// mail program for mailto. The launcher stands behind a seam like the clipboard, so a widget that
// opens addresses (`ui::Link`) is tested with a fake, and an application can put its own in.
//
// The system launcher opens only what cannot start a program: http, https and mailto. Windows'
// ShellExecute and xdg-open run whatever they are given, and an address can come from outside (a
// page, a file, a source on the network), so `file:` or a scheme of some program's own would be a
// way to start that program with one click. Such an address, and every address over ssh or without
// a graphical session, goes to the clipboard instead: the browser of this machine is not the user's
// over ssh, and without a session there is no window to open it in; from the clipboard the user
// takes it where they want. An address with a control character is no address at all and is
// refused.
namespace talcite {

namespace {

// Whether every byte of the address may stand in one: nothing below a space and no DEL. A line
// feed or an escape in an address is not a typo but a sign that it was made to do something else.
bool sound(std::string_view address) {
    if (address.empty()) return false;

    for (const char letter : address) {
        const auto one = static_cast<unsigned char>(letter);

        if (one < 0x20 || one == 0x7F) return false;
    }

    return true;
}

// Whether the address starts with the scheme, letter case aside, as schemes are compared.
bool schemed(std::string_view address, std::string_view scheme) {
    if (address.size() < scheme.size()) return false;

    for (std::size_t at = 0; at < scheme.size(); ++at) {
        char one = address[at];

        if (one >= 'A' && one <= 'Z') one = static_cast<char>(one - 'A' + 'a');

        if (one != scheme[at]) return false;
    }

    return true;
}

// Whether the system may open the address: a web page or a mail address, nothing that names a
// program or a file.
bool openable(std::string_view address) {
    return schemed(address, "http://") || schemed(address, "https://") || schemed(address, "mailto:");
}

// Whether there is a graphical session to open the address in: always on Windows and macOS, and
// elsewhere when X or Wayland says so. Without one xdg-open would reach for a text browser, which
// has no terminal of its own here and would fight us for ours.
bool windowed(const Console& from) {
#if defined(_WIN32) || defined(__APPLE__)

    (void)from;

    return true;

#else

    return from.variable("DISPLAY").has_value() || from.variable("WAYLAND_DISPLAY").has_value();

#endif
}

// The address on the clipboard: copied when the clipboard took it, refused otherwise.
Launched copied(const Seam<Clipboard>& copying, std::string_view address) {
    return copying.provider().copy(address) ? Launched::copied : Launched::refused;
}

#ifdef _WIN32

// Hands the address to the shell, which opens it in the program registered for its scheme; true
// when the shell took it. ShellExecute may reach the program through a shell extension, which is
// COM, so the thread has COM for the time of the call, in a single-threaded apartment as the shell
// wants it, and without OLE1 DDE, which the shell does not need for an address. It waits for the
// shell, not for the browser, but even that is too long for the loop, so it runs on the pool.
bool shelled(const std::string& address) {
    const HRESULT joined = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const std::wstring wide = to_wstring(address);

    const auto told = reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));

    if (SUCCEEDED(joined)) CoUninitialize();

    return told > 32;
}

#else

// The program that opens an address in the session: `open` on macOS, xdg-open elsewhere.
Command opening(const std::string& address) {
    Command made;

#ifdef __APPLE__
    made.program = "/usr/bin/open";
#else
    made.program = "xdg-open";
#endif

    made.arguments = {address};

    return made;
}

#endif

// The launcher of the user's system, with the console it learns the session from and the clipboard
// that takes what it does not open.
class System final : public Launcher {
public:
    System(Seam<Console> with, Seam<Clipboard> copying) : console_(std::move(with)), copying_(std::move(copying)) {}

    // The launcher's name in messages.
    [[nodiscard]] std::string_view name() const override { return "system launcher"; }

    // Opens the address, copies it, or refuses it, as the head of this file says. What is decided
    // at once is decided here, on the caller's thread, because a copy over ssh writes to the
    // terminal, and only the loop's thread writes there. The program that opens the address is
    // started on the pool and let loose: a browser may stay as long as it likes, and it is not ours
    // to wait for. When it cannot start (no xdg-open, the shell refused), the address goes to the
    // clipboard after all, back on the caller's loop.
    [[nodiscard]] Promise<Launched> open(std::string address) override {
        if (!sound(address)) return ready(Launched::refused);

        const Console& from = console_.provider();

        if (remote(from) || !openable(address) || !windowed(from)) return ready(copied(copying_, address));

#ifdef _WIN32

        return run(io(), [address] { return shelled(address); })
            .here()
            .then([copying = copying_, address](bool done) { return done ? Launched::opened : copied(copying, address); });

#else

        return Child::detached(opening(address))
            .here()
            .then([](std::int64_t) { return Launched::opened; })
            .recover([copying = copying_, address](const std::exception&) { return copied(copying, address); });

#endif
    }

private:
    Seam<Console> console_;
    Seam<Clipboard> copying_;
};

}

// The launcher of the user's system, on the program's console and its shared clipboard (`clipboard`),
// so that an address it copies is the same copy an editor would paste. One per process: it holds
// nothing but the two seams.
Seam<Launcher> launcher() {
    static const Seam<Launcher> shared{std::make_shared<System>(console(), clipboard())};

    return shared;
}

// A launcher that learns the session from the given console and copies to the given clipboard: a
// second terminal, or a trial.
Seam<Launcher> launcher(Seam<Console> with, Seam<Clipboard> copying) {
    return Seam<Launcher>{std::make_shared<System>(std::move(with), std::move(copying))};
}

}
