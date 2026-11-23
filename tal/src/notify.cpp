#include "tal/notify.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "notifying.hpp"

#ifdef _WIN32
#include <future>
#include <memory>
#include <optional>
#include <mutex>
#include <thread>

#include <windows.h>

#include <shellapi.h>
#endif

// Notifications of the system, for a program with no terminal to tell it through (a daemon) and for
// a terminal that has no notifications of its own. Linux and the other systems of freedesktop ask
// the notification service on the session's D-Bus through gdbus, which comes with GLib on every
// desktop, and fall back to notify-send; macOS asks AppleScript (osascript). Each runs as a program
// of its own through `Processes`, with no library of D-Bus linked, and is given `patience` to end,
// as the clipboard's programs are. Windows shows a balloon of the notification area (a toast since
// Windows 10), whose icon stands there only while the balloon is shown.
//
// What is told reaches the program as its arguments, never through a shell: the title and the body
// are written as literals of the language that reads them (GVariant's text form, AppleScript's
// strings), every quote and backslash escaped and every control character spelled out, so that no
// text can end its literal and say something of its own.
namespace tal {

namespace {

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds patience = 1500ms;

#if defined(__APPLE__)
constexpr bool apple_here = true;
#elif !defined(_WIN32)
constexpr bool apple_here = false;
#endif

// Runs `what` through `processes`; true when it ended well within `patience`. A program that is not
// there or cannot start is false, and so is one that is stuck, which is killed.
[[nodiscard]] Task<bool> ran(std::shared_ptr<Processes> processes, Command what) {
    Process child;

    try {
        child = co_await processes->start(std::move(what));
    } catch (const Trouble&) {
        co_return false;
    }

    try {
        const Exit made = co_await within(patience, child.ended());

        co_return made.ended == Exit::Ended::normal && made.code == 0;
    } catch (const Trouble&) {
        child.kill();

        co_return false;
    }
}

// A command of `program` with `arguments`.
[[nodiscard]] Command commanded(std::string_view program, std::vector<std::string> arguments) {
    Command made;

    made.program = Path{std::string{program}};
    made.arguments = std::move(arguments);

    return made;
}

// The urgency as the specification of notifications counts it: 0, 1 or 2.
[[nodiscard]] int level(Urgency urgency) {
    switch (urgency) {
    case Urgency::low: return 0;
    case Urgency::critical: return 2;
    case Urgency::normal: break;
    }

    return 1;
}

// The arguments of osascript that show `notice`: one line of AppleScript, its texts as literals.
[[nodiscard]] std::vector<std::string> displaying(const Notice& notice) {
    return {"-e", "display notification " + notifying::applescript(notice.body) + " with title " + notifying::applescript(notice.title)};
}

// The arguments of gdbus that call Notify of the service of notifications with `notice`: no name of
// a program, none to replace, no icon, the title and the body, no actions, the urgency as a hint
// and the server's own time to show it (-1, typed, as gdbus would read a bare -1 as an option).
[[nodiscard]] std::vector<std::string> asking(const Notice& notice) {
    return {"call", "--session", "--dest", "org.freedesktop.Notifications", "--object-path", "/org/freedesktop/Notifications", "--method",
            "org.freedesktop.Notifications.Notify", "''", "0", "''", notifying::gvariant(notice.title), notifying::gvariant(notice.body), "[]",
            "{'urgency': <byte " + std::to_string(level(notice.urgency)) + ">}", "int32 -1"};
}

// The arguments of notify-send for `notice`: its urgency, then the title and the body after `--`,
// so that neither is read as an option.
[[nodiscard]] std::vector<std::string> sent_as(const Notice& notice) {
    const std::string urgency = notice.urgency == Urgency::low ? "low" : notice.urgency == Urgency::critical ? "critical" : "normal";

    return {"--urgency=" + urgency, "--", notice.title, notice.body};
}

// The notifications of the system, through the programs `processes` starts: AppleScript's when
// `apple`, those of freedesktop otherwise.
class System final : public Notifier {
public:
    // Over the processes that run the programs of notifications, of macOS when `apple`.
    System(Port<Processes> processes, bool apple) : processes_(std::move(processes)), apple_(apple) {}

    // The name of the notifier.
    [[nodiscard]] std::string_view name() const override { return "system"; }

    // Shows `notice` (see above), its texts made UTF-8, which every one of the programs demands.
    // Throws `Trouble` unsupported when no program of notifications takes it.
    [[nodiscard]] Task<void> tell(Notice notice) override {
        notice.title = text::repaired(notice.title);
        notice.body = text::repaired(notice.body);

        if (apple_) {
            const Command display = commanded("osascript", displaying(notice));
            const bool shown = co_await ran(processes_.shared(), display);

            if (shown) co_return;

            throw Trouble{Kind::unsupported, "No program here shows notifications."};
        }

        const Command asked = commanded("gdbus", asking(notice));
        const bool asked_well = co_await ran(processes_.shared(), asked);

        if (asked_well) co_return;

        const Command sending = commanded("notify-send", sent_as(notice));
        const bool sent = co_await ran(processes_.shared(), sending);

        if (sent) co_return;

        throw Trouble{Kind::unsupported, "No program here shows notifications."};
    }

private:
    Port<Processes> processes_;
    bool apple_;
};

#ifdef _WIN32

constexpr UINT called_back = WM_APP + 1;
constexpr UINT handed = WM_APP + 2;
constexpr WORD information_icon = 32516;
constexpr UINT longest_shown = 10 * 60 * 1000;
constexpr wchar_t class_name[] = L"tal.notifier";

// `text` as UTF-16 in `into`, of `room` characters with the closing zero: cut where it does not
// fit, never between the two halves of a pair of surrogates.
template <std::size_t room>
void fitted(wchar_t (&into)[room], std::string_view text) {
    const std::wstring wide = text::wide(text);
    std::size_t much = std::min(wide.size(), room - 1);

    if (much > 0 && much < wide.size() && wide[much - 1] >= 0xD800 && wide[much - 1] <= 0xDBFF) --much;

    wide.copy(into, much);
    into[much] = L'\0';
}

// A notice handed to the thread of the window, and its word on whether the shell took it.
struct Asked {
    Notice notice;
    Pending<bool> shown;
};

// The balloon of the notification area (a toast since Windows 10), with the program's own icon.
// The shell asks for a window to tell what became of a balloon, so a window of messages alone is
// made, on the first notice, by a thread of its own that hands it its messages until the notifier
// ends. That thread alone speaks to the shell, so that what the shell says back meets no lock: a
// notice reaches it as a message, and `tell` awaits its word, `patience` at most, holding no
// thread meanwhile.
//
// Every notice has an icon of its own, added for it and taken away when its balloon goes (its time
// ran out, it was clicked or closed), which the shell tells by the icon's number; so a balloon that
// goes late never takes away a later one, and notices that come together are all shown, as the
// system queues them. An icon whose balloon the shell never speaks of again is taken away after
// `longest_shown`, and every icon when the notifier ends.
class Balloon final : public Notifier {
public:
    // Closes the window, which takes away the icons still shown, and ends the thread.
    ~Balloon() override {
        if (window_ != nullptr) PostMessageW(window_, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }

    // The name of the notifier.
    [[nodiscard]] std::string_view name() const override { return "system"; }

    // Shows `notice` (see above). Throws `Trouble` unsupported when the system has no window for
    // it or no notification area, or does not answer in time.
    [[nodiscard]] Task<void> tell(Notice notice) override {
        const HWND window = opened();

        if (window == nullptr) throw Trouble{Kind::unsupported, "This system has no window for notifications."};

        Executor& back = co_await detail::Here{};
        auto asked = std::make_unique<Asked>();
        const Pending<bool> shown = asked->shown;

        asked->notice = std::move(notice);

        if (PostMessageW(window, handed, 0, reinterpret_cast<LPARAM>(asked.get())) == FALSE)
            throw Trouble{Kind::unsupported, "This system has no window for notifications."};

        (void)asked.release();

        std::optional<bool> answer;

        try {
            answer = co_await within(patience, shown.task());
        } catch (const Overdue&) {
            answer.reset();
        }

        co_await to(back);

        if (!answer) throw Trouble{Kind::unsupported, "The notification area does not answer."};

        if (!*answer) throw Trouble{Kind::unsupported, "This system has no notification area."};
    }

private:
    // The window, made with its thread on the first call; null when none could be made.
    [[nodiscard]] HWND opened() {
        const std::lock_guard<std::mutex> kept{guard_};

        if (thread_.joinable()) return window_;

        std::promise<HWND> made;
        std::future<HWND> coming = made.get_future();

        thread_ = std::thread{[this, made = std::move(made)]() mutable { looped(std::move(made)); }};
        window_ = coming.get();

        return window_;
    }

    // Makes the window and hands it its messages until it is closed; a notice still waiting then is
    // answered as not shown.
    void looped(std::promise<HWND> made) {
        WNDCLASSEXW shape{};

        shape.cbSize = sizeof shape;
        shape.lpfnWndProc = &Balloon::received;
        shape.hInstance = GetModuleHandleW(nullptr);
        shape.lpszClassName = class_name;

        if (RegisterClassExW(&shape) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            made.set_value(nullptr);

            return;
        }

        const HWND window = CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, shape.hInstance, this);

        if (window != nullptr) icon_ = drawn();

        made.set_value(window);

        if (window == nullptr) return;

        MSG message;

        while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);

        while (PeekMessageW(&message, nullptr, handed, handed, PM_REMOVE) != FALSE) {
            const std::unique_ptr<Asked> asked{reinterpret_cast<Asked*>(message.lParam)};

            asked->shown.settle(false);
        }

        if (own_icon_) DestroyIcon(icon_);
    }

    // The small icon of the program's own file, or the system's icon of information when the file
    // has none; the first is ours to destroy.
    [[nodiscard]] HICON drawn() {
        std::wstring file(MAX_PATH, L'\0');

        for (;;) {
            const DWORD much = GetModuleFileNameW(nullptr, file.data(), static_cast<DWORD>(file.size()));

            if (much == 0) break;

            if (much < file.size()) {
                file.resize(much);

                HICON icon = nullptr;

                if (ExtractIconExW(file.c_str(), 0, nullptr, &icon, 1) > 0 && icon != nullptr) {
                    own_icon_ = true;

                    return icon;
                }

                break;
            }

            file.resize(file.size() * 2);
        }

        return LoadIconW(nullptr, MAKEINTRESOURCEW(information_icon));
    }

    // The data of the icon numbered `id` on `window`, with nothing to change yet.
    [[nodiscard]] static NOTIFYICONDATAW about(HWND window, UINT id) {
        NOTIFYICONDATAW data{};

        data.cbSize = sizeof data;
        data.hWnd = window;
        data.uID = id;

        return data;
    }

    // A number for a new icon: from 1 to 65535, since the shell tells it in the high word of a
    // message, and none an icon shown still has.
    [[nodiscard]] UINT numbered() {
        for (;;) {
            const UINT id = next_;

            next_ = next_ == 0xFFFF ? 1 : next_ + 1;

            if (std::find(shown_.begin(), shown_.end(), id) == shown_.end()) return id;
        }
    }

    // Shows the notice of `asked` on the thread of the window and gives the shell's word: a new
    // icon is added with its tip and its callback, told to speak as version 4 does (the icon's
    // number with every event), and only then given the balloon, which version 4 shows as it
    // should.
    void showed(HWND window, Asked& asked) {
        const UINT id = numbered();

        NOTIFYICONDATAW data = about(window, id);
        const Notice& notice = asked.notice;

        data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
        data.uCallbackMessage = called_back;
        data.hIcon = icon_;
        fitted(data.szTip, notice.title);

        if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
            asked.shown.settle(false);

            return;
        }

        data.uVersion = NOTIFYICON_VERSION_4;
        (void)Shell_NotifyIconW(NIM_SETVERSION, &data);

        data.uFlags = NIF_INFO;
        data.dwInfoFlags = notice.urgency == Urgency::critical ? NIIF_WARNING : NIIF_INFO;

        if (notice.urgency == Urgency::low) data.dwInfoFlags |= NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;

        fitted(data.szInfoTitle, notice.title);
        fitted(data.szInfo, notice.body);

        if (Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE) {
            (void)Shell_NotifyIconW(NIM_DELETE, &data);
            asked.shown.settle(false);

            return;
        }

        shown_.push_back(id);
        (void)SetTimer(window, id, longest_shown, nullptr);
        asked.shown.settle(true);
    }

    // Takes away the icon numbered `id` when it is shown, on the thread of the window.
    void hidden(HWND window, UINT id) {
        const auto at = std::find(shown_.begin(), shown_.end(), id);

        if (at == shown_.end()) return;

        shown_.erase(at);
        (void)KillTimer(window, id);

        NOTIFYICONDATAW data = about(window, id);

        (void)Shell_NotifyIconW(NIM_DELETE, &data);
    }

    // The window's messages: a notice handed to it is shown, the shell's word that a balloon went
    // and the time of an icon running out take its icon away, closing takes every icon away and
    // destroys the window, and destroying ends the thread's loop.
    static LRESULT CALLBACK received(HWND window, UINT message, WPARAM word, LPARAM value) {
        if (message == WM_NCCREATE) {
            const auto* creating = reinterpret_cast<const CREATESTRUCTW*>(value);

            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creating->lpCreateParams));

            return DefWindowProcW(window, message, word, value);
        }

        auto* self = reinterpret_cast<Balloon*>(GetWindowLongPtrW(window, GWLP_USERDATA));

        if (self == nullptr) return DefWindowProcW(window, message, word, value);

        if (message == handed) {
            const std::unique_ptr<Asked> asked{reinterpret_cast<Asked*>(value)};

            self->showed(window, *asked);

            return 0;
        }

        if (message == called_back) {
            const UINT what = LOWORD(value);

            if (what == NIN_BALLOONTIMEOUT || what == NIN_BALLOONUSERCLICK || what == NIN_BALLOONHIDE) self->hidden(window, HIWORD(value));

            return 0;
        }

        if (message == WM_TIMER) {
            self->hidden(window, static_cast<UINT>(word));

            return 0;
        }

        if (message == WM_CLOSE) {
            while (!self->shown_.empty()) self->hidden(window, self->shown_.back());

            DestroyWindow(window);

            return 0;
        }

        if (message == WM_DESTROY) {
            PostQuitMessage(0);

            return 0;
        }

        return DefWindowProcW(window, message, word, value);
    }

    std::mutex guard_;
    std::thread thread_;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    bool own_icon_ = false;
    UINT next_ = 1;
    std::vector<UINT> shown_;
};

#endif

}

// The text as a string literal of GVariant's text form: in single quotes, a quote or a backslash
// after a backslash, a control character (and DEL) as \uXXXX.
std::string notifying::gvariant(std::string_view text) {
    std::string made = "'";

    for (const char one : text) {
        const auto code = static_cast<unsigned char>(one);

        if (one == '\'' || one == '\\') {
            made += '\\';
            made += one;
        } else if (code < 0x20 || code == 0x7F) {
            char escaped[8];

            std::snprintf(escaped, sizeof escaped, "\\u%04x", code);
            made += escaped;
        } else {
            made += one;
        }
    }

    return made + "'";
}

// The text as a string literal of AppleScript: in double quotes, a quote or a backslash after a
// backslash, a line break as \n, a tab as \t, and the other control characters left out, as a
// notification shows none of them.
std::string notifying::applescript(std::string_view text) {
    std::string made = "\"";

    for (const char one : text) {
        const auto code = static_cast<unsigned char>(one);

        if (one == '"' || one == '\\') {
            made += '\\';
            made += one;
        } else if (one == '\n') {
            made += "\\n";
        } else if (one == '\t') {
            made += "\\t";
        } else if (code >= 0x20 && code != 0x7F) {
            made += one;
        }
    }

    return made + "\"";
}

// The notifications of the programs of macOS when `apple`, of freedesktop otherwise, run through
// `processes`: what the system uses, and what the trials give a scripted shell.
Port<Notifier> notifying::programs(Port<Processes> processes, bool apple) {
    return Port<Notifier>{std::make_shared<System>(std::move(processes), apple)};
}

// The notifications of the system (see above), over the default processes. On Windows one balloon
// serves the whole program: a notifier made for each notice would end with it, and its balloon too.
std::shared_ptr<Notifier> Notifier::builtin() {
#ifdef _WIN32
    static const std::shared_ptr<Balloon> one = std::make_shared<Balloon>();

    return one;
#else
    return notifying::programs(Port<Processes>{}, apple_here).shared();
#endif
}

// Tells `notice` through `notifier`.
Task<void> tell(Notice notice, Port<Notifier> notifier) { co_await notifier->tell(std::move(notice)); }

// The notifications of the system with its programs run through `processes`; Windows needs none
// and gets a balloon of its own, whose icons go when the last copy of the port does.
Port<Notifier> notifier(Port<Processes> processes) {
#ifdef _WIN32
    (void)processes;

    return Port<Notifier>{std::make_shared<Balloon>()};
#else
    return notifying::programs(std::move(processes), apple_here);
#endif
}

// Keeps the notice as told.
Task<void> ScriptedNotifier::tell(Notice notice) {
    told.push_back(std::move(notice));

    co_return;
}

}
