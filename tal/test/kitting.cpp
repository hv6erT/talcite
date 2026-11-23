#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

#include "tal/docs.hpp"
#include "tal/executor.hpp"
#include "tal/process.hpp"
#include "tal/net.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "locating.hpp"
#include "tal/testing.hpp"

// The web engine of tal, run for real beside the trials when tal is built with one: WebKitGTK 6 on
// Linux (tal-render-gtk, TAL_WEBKITGTK) or WebView2 on Windows (tal-render-webview2, TAL_WEBVIEW2):
// a local page shown at its size, typed into and its form sent with Enter, a link to the web
// refused and told, back and forward, a resize, a page of bytes 10 (LF) shot whole; and a page that
// reaches for a tracking server on the loopback in every way a page can, which with the network off
// reaches it in none (scripts on or off), while the network on shows the trial sees.

#if defined(TAL_WEBKITGTK) || defined(TAL_WEBVIEW2)

namespace {

using tal::Command;
using tal::docs::Browsing;
using tal::docs::Change;
using tal::media::Size;

// The program of the engine built in, beside the trials.
tal::Path engine_program() {
#ifdef TAL_WEBVIEW2
    return tal::testing::mine().parent() / "tal-render-webview2";
#else
    return tal::testing::mine().parent() / "tal-render-gtk";
#endif
}

// The limits of the trials' engines: a minute to greet instead of ten seconds, since under load
// beside other trials and programs several engines starting at once take longer than a program
// waits for one.
tal::docs::Limits patient() {
    tal::docs::Limits made;

    made.greeting = std::chrono::milliseconds{60000};

    return made;
}

// The engine built in, as its adapter: as tal gives it, except under the sanitizers, whose own
// reservations of memory its limit would count; there the same program, without the limit.
std::shared_ptr<tal::docs::Browser> engine() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    tal::Command program;

    program.program = engine_program();

    return tal::docs::browser(program, patient());
#elif defined(TAL_WEBVIEW2)
    return tal::docs::webview2(patient());
#else
    return tal::docs::webkitgtk(patient());
#endif
}

// A folder of the trial's own pages.
std::filesystem::path pages() {
    const auto mark = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path made = std::filesystem::temp_directory_path() / ("tal-kitting-" + std::to_string(mark));

    std::filesystem::create_directories(made);

    return made;
}

// The page `name` of `html` in `folder`, as a file address.
std::string written(const std::filesystem::path& folder, const std::string& name, const std::string& html) {
    std::ofstream{folder / name, std::ios::binary} << html;

#ifdef _WIN32
    return "file:///" + (folder / name).generic_string();
#else
    return tal::locating::located(tal::Path{(folder / name).string()});
#endif
}

// A server on the loopback that only counts the connections it is offered.
class Tracker {
public:
    // Listening on the loopback of `loop`.
    explicit Tracker(tal::Loop& loop)
        : listener_(tal::wait(loop, tal::net::Listener::opened(tal::net::Address::loopback(tal::net::Family::four, 0)))), scope_(loop) {
        scope_.run(accepting());
    }

    // Stops listening.
    ~Tracker() { listener_.close(); }

    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

    // Its address, "http://127.0.0.1:<port>".
    [[nodiscard]] std::string address() const { return "http://127.0.0.1:" + std::to_string(listener_.mine().port()); }

    std::size_t connections = 0;

private:
    // Counts the connections as they come, and lets them go.
    tal::Task<void> accepting() {
        try {
            for (;;) {
                tal::net::Connection one = co_await listener_.accepted();

                ++connections;
                (void)one;
            }
        } catch (const tal::Trouble&) {
        }
    }

    tal::net::Listener listener_;
    tal::Scope scope_;
};

// The loop turned for `much`, real time.
void lingered(tal::Loop& loop, std::chrono::milliseconds much) {
    (void)tal::wait(loop, [](std::chrono::milliseconds wait) -> tal::Task<int> {
        co_await tal::after(wait);
        co_return 0;
    }(much));
}

// The next change of `view` after `after` that `wanted` likes, within a minute: as for greeting
// (`patient`), an engine under load beside other trials takes longer than a program waits.
template <typename Wanted>
Change awaited(tal::Loop& loop, tal::docs::Browsed& view, std::uint64_t after, Wanted wanted) {
    const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{60};

    for (;;) {
        std::vector<tal::Task<Change>> racing;

        racing.push_back(view.changed(after));
        racing.push_back([](std::chrono::steady_clock::time_point until) -> tal::Task<Change> {
            co_await tal::after(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()));

            Change none;

            none.title = "(nothing came in a minute)";
            co_return none;
        }(latest));

        Change made = tal::wait(loop, tal::first(std::move(racing)));

        if (made.sequence == 0) tal::testing::fell(__FILE__, __LINE__, made.title);

        if (wanted(made)) return made;

        if (std::chrono::steady_clock::now() > latest) tal::testing::fell(__FILE__, __LINE__, "the change never came: " + made.title + " " + made.address);

        after = made.sequence;
    }
}

}

TRIAL("Web engine: a local page shown, typed into, its form sent; a link to the web refused; back; resized") {
    tal::Loop loop;
    const std::filesystem::path folder = pages();
    const std::string next = written(folder, "next.html", "<title>Next</title><p>Sent.");
    const std::string first = written(folder, "first.html",
                                      "<title>First</title><body style='margin:0'><form action='next.html'><input name=q autofocus "
                                      "style='position:absolute;left:0;top:0;width:200px;height:40px'></form>"
                                      "<a href='http://example.org/' style='position:absolute;left:0;top:100px;width:100px;height:50px;display:block'>web</a>");
    const auto browser = engine();
    const auto view = tal::wait(loop, browser->opened(first, Browsing{}, Size{320, 200}));
    const tal::docs::Shot shot = tal::wait(loop, view->shot());

    OUGHT(!tal::wait(loop, view->changed(0)).loading);

    Change seen = awaited(loop, *view, 0, [](const Change& one) { return one.title == "First" && !one.loading; });

    OUGHT(shot.pixels.size() == (Size{320, 200}));

    tal::wait(loop, view->clicked(50, 20));
    tal::wait(loop, view->typed("abc"));
    tal::wait(loop, view->keyed(tal::Key{tal::Code::enter}));
    seen = awaited(loop, *view, seen.sequence, [](const Change& one) { return one.title == "Next" && !one.loading; });
    SAME(seen.address, next + "?q=abc");
    OUGHT(seen.back);

    tal::wait(loop, view->stepped(tal::docs::Step::back));
    seen = awaited(loop, *view, seen.sequence, [](const Change& one) { return one.title == "First" && !one.loading; });

    tal::wait(loop, view->clicked(50, 125));
    seen = awaited(loop, *view, seen.sequence, [](const Change& one) { return !one.refusals.empty(); });
    SAME(seen.refusals.front().address, std::string{"http://example.org/"});
    OUGHT(seen.refusals.front().reason == tal::docs::Refusal::Reason::outside);
    SAME(seen.title, std::string{"First"});

    SAME(tal::spoken(tal::Kind::refused), std::string_view{"refused"});
    THREW(tal::wait(loop, view->went("https://example.org/")));

    lingered(loop, std::chrono::milliseconds{1500});
    tal::wait(loop, view->resized(Size{400, 300}));
    OUGHT(tal::wait(loop, view->shot()).pixels.size() == (Size{400, 300}));

    const std::string dark = written(folder, "dark.html", "<title>Dark</title><html style='background:#0a0a0a'><body style='margin:0'>");

    tal::wait(loop, view->went(dark));
    seen = awaited(loop, *view, seen.sequence, [](const Change& one) { return one.title == "Dark" && !one.loading; });

    const tal::docs::Shot lines = tal::wait(loop, view->shot());
    const tal::Bytes& rgba = lines.pixels.stills().front().rgba;
    std::size_t other = 0;

    for (std::size_t at = 0; at + 4 <= rgba.size(); at += 4) {
        const bool newline = std::to_integer<int>(rgba[at]) == 10 && std::to_integer<int>(rgba[at + 1]) == 10 && std::to_integer<int>(rgba[at + 2]) == 10;

        other += newline && std::to_integer<int>(rgba[at + 3]) == 255 ? 0 : 1;
    }

    SAME(rgba.size(), std::size_t{400} * 300 * 4);
    SAME(other, std::size_t{0});

    const std::string form = written(folder, "form.html",
                                     "<title>Form</title><body style='margin:0'><form action='next.html'>"
                                     "<select name=s style='position:absolute;left:0;top:0;width:100px;height:30px'><option>a<option>b<option>c</select>"
                                     "<textarea name=t style='position:absolute;left:0;top:50px;width:200px;height:60px'></textarea>"
                                     "<button style='position:absolute;left:0;top:150px;width:100px;height:30px'>Send</button></form>");

    tal::wait(loop, view->went(form));
    seen = awaited(loop, *view, seen.sequence, [](const Change& one) { return one.title == "Form" && !one.loading; });
    tal::wait(loop, view->clicked(50, 15));
    tal::wait(loop, view->keyed(tal::Key{tal::Code::down}));
    tal::wait(loop, view->keyed(tal::Key{tal::Code::down}));
    tal::wait(loop, view->clicked(50, 80));
    tal::wait(loop, view->typed("x"));
    tal::wait(loop, view->keyed(tal::Key{tal::Code::enter}));
    tal::wait(loop, view->typed("y"));
    tal::wait(loop, view->clicked(50, 165));
    seen = awaited(loop, *view, seen.sequence, [](const Change& one) { return one.title == "Next" && !one.loading; });
    SAME(seen.address, next + "?s=c&t=x%0D%0Ay");

    const auto zoomed = tal::wait(loop, browser->opened(form, Browsing{}, Size{400, 400}));
    Change zoom_seen = awaited(loop, *zoomed, 0, [](const Change& one) { return one.title == "Form" && !one.loading; });

    tal::wait(loop, zoomed->zoomed(2));
    tal::wait(loop, zoomed->clicked(150, 30));
    tal::wait(loop, zoomed->keyed(tal::Key{tal::Code::down}));
    tal::wait(loop, zoomed->clicked(100, 330));
    zoom_seen = awaited(loop, *zoomed, zoom_seen.sequence, [](const Change& one) { return one.title == "Next" && !one.loading; });
    SAME(zoom_seen.address, next + "?s=b&t=");

    tal::wait(loop, view->went(next + "#part"));
    seen = awaited(loop, *view, seen.sequence, [&](const Change& one) { return one.address == next + "#part"; });

    std::error_code ignored;

    std::filesystem::remove_all(folder, ignored);
}

TRIAL("Web engine: with the network off a page reaches a tracker in no way, scripts on or off; with it on the trial sees it does") {
    tal::Loop loop;
    Tracker tracker{loop};
    const std::string t = tracker.address();
    const std::string ws = "ws" + t.substr(4);
    const std::filesystem::path folder = pages();
    const std::string local_png = (folder / "dot.svg").generic_string();

    std::ofstream{folder / "dot.svg", std::ios::binary} << "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'/>";
    const std::string html = "<title>Vectors</title>"
                             "<meta http-equiv=refresh content='0;url=" + t + "/refresh'>"
                             "<link rel=stylesheet href='" + t + "/style.css'><link rel=dns-prefetch href='" + t + "'>"
                             "<link rel=preconnect href='" + t + "'><link rel=prefetch href='" + t + "/prefetch'><link rel=preload href='" + t + "/preload' as=image>"
                             "<style>@import url('" + t + "/import.css'); @font-face{font-family:x;src:url('" + t + "/font.woff')}"
                             "body{background:url('" + t + "/back.png');font-family:x}</style>"
                             "<img src='" + t + "/img.png'><img srcset='" + t + "/srcset.png 2x'><video poster='" + t + "/poster.png'></video>"
                             "<svg><image href='" + t + "/svg.png' width=10 height=10 /></svg><iframe src='" + t + "/frame'></iframe>"
                             "<object data='" + t + "/object'></object><img src='file://127.0.0.2/share/unc.png'>"
                             "<img src='file://127.0.0.2" + local_png + "' onload=\"document.title='Leaked'\">"
                             "<script>fetch('" + t + "/fetch').catch(()=>{});try{new WebSocket('" + ws + "/ws')}catch(e){}"
                             "navigator.sendBeacon('" + t + "/beacon')</script><p>text";
    const std::string page = written(folder, "vectors.html", html);
    const auto browser = engine();

    for (const bool scripts : {false, true}) {
        const auto view = tal::wait(loop, browser->opened(page, Browsing{scripts, false}, Size{320, 200}));

        (void)awaited(loop, *view, 0, [](const Change& one) { return one.title == "Vectors" && !one.loading; });
        lingered(loop, std::chrono::milliseconds{1500});
        SAME(tracker.connections, std::size_t{0});
        SAME(tal::wait(loop, view->changed(0)).title, std::string{"Vectors"});
    }

    const auto open = tal::wait(loop, browser->opened(page, Browsing{true, true}, Size{320, 200}));

    lingered(loop, std::chrono::milliseconds{1500});
    OUGHT(tracker.connections > 0);

    std::error_code ignored;

    std::filesystem::remove_all(folder, ignored);
}

#ifdef TAL_WEBKITGTK

namespace {

// The parent of the process `number`, or 0.
long parent_of(long number) {
    std::ifstream status{"/proc/" + std::to_string(number) + "/status"};

    for (std::string line; std::getline(status, line);)
        if (line.starts_with("PPid:")) return std::stol(line.substr(5));

    return 0;
}

// The processes of WebKit's pages under ours killed, as a crash or the kernel would kill them;
// how many.
std::size_t pages_killed() {
    std::size_t made = 0;
    std::error_code wrong;
    const long ours = ::getpid();

    for (const auto& one : std::filesystem::directory_iterator{"/proc", wrong}) {
        const std::string name = one.path().filename().string();

        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;

        std::string command;

        std::getline(std::ifstream{one.path() / "comm"}, command);

        if (command != "WebKitWebProces") continue;

        for (long up = std::stol(name); up > 1; up = parent_of(up)) {
            if (up != ours) continue;

            ::kill(static_cast<pid_t>(std::stol(name)), SIGKILL);
            ++made;

            break;
        }
    }

    return made;
}

}

// A page whose process dies (JavaScriptCore turns running out of memory into an exception of the
// page, so the trial kills it as a crash would) is lost, said so; a new view opens after it in
// the same engine. An engine killed leaves its folder, which the next one sweeps away. The
// engines of the trial keep their folders in a runtime folder of its own, so that another run of
// the trials beside it counts in none of them.
TRIAL("Web engine: a page whose process dies is lost and the next view opens; the folder of a killed engine swept away") {
    tal::Loop loop;
    const std::filesystem::path folder = pages();
    const std::string doomed = written(folder, "doomed.html", "<title>Doomed</title>");
    const std::string plain = written(folder, "plain.html", "<title>Plain</title>");
    Command program;

    const std::filesystem::path runtime = folder / "runtime";

    std::filesystem::create_directories(runtime);
    std::filesystem::permissions(runtime, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    program.program = engine_program();
    program.environment.push_back(tal::Variable{"XDG_RUNTIME_DIR", runtime.string()});
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    program.memory = std::uint64_t{2} << 30;
#endif

    const auto browser = tal::docs::browser(program, patient());
    const auto view = tal::wait(loop, browser->opened(doomed, Browsing{}, Size{100, 100}));
    const std::uint64_t seen = awaited(loop, *view, 0, [](const Change& one) { return one.title == "Doomed"; }).sequence;

    OUGHT(pages_killed() > 0);
    (void)awaited(loop, *view, seen, [](const Change& one) { return one.lost; });

    const auto next = tal::wait(loop, browser->opened(plain, Browsing{}, Size{100, 100}));

    (void)awaited(loop, *next, 0, [](const Change& one) { return one.title == "Plain"; });

    const auto ours = [&runtime] {
        std::size_t made = 0;
        std::error_code wrong;

        for (const auto& one : std::filesystem::directory_iterator{runtime, wrong})
            if (one.path().filename().string().starts_with("tal-render-gtk-")) ++made;

        return made;
    };
    const std::size_t living = ours();

    {
        const auto killed = tal::docs::browser(program, patient());

        (void)tal::wait(loop, killed->opened(plain, Browsing{}, Size{10, 10}));
    }

    SAME(ours(), living + 1);

    {
        const auto fresh = tal::docs::browser(program, patient());

        (void)tal::wait(loop, fresh->opened(plain, Browsing{}, Size{10, 10}));
        SAME(ours(), living + 1);
    }

    std::error_code ignored;

    std::filesystem::remove_all(folder, ignored);
}

#endif

#endif
