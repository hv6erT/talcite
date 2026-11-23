#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "tal/docs.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/executor.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/scene.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"
#include "tal/webview.hpp"

#include "locating.hpp"
#include "quieting.hpp"
#include "tal/testing.hpp"

// Pages of the web in the tree (E6e), on the scripted browser: clicks land on the middle pixels of
// their cells, the wheel scrolls three rows, keys and text reach the page while it has the focus,
// Ctrl stays the program's; the changes the view tells come as Visited and Refused; a new size
// resizes the view; a trouble shows in place of the page and the next input opens it anew; with
// no engine, or no pixels on the terminal, the text version of a local page under the reason.

namespace {

using namespace std::chrono_literals;

using tal::Code;
using tal::Display;
using tal::Document;
using tal::Graphics;
using tal::Key;
using tal::Mods;
using tal::Size;
using tal::Spot;
using tal::Webview;
using tal::docs::Refusal;
using tal::docs::ScriptedBrowser;
using tal::testing::Scene;

// A terminal of kitty pictures, cells of 10 by 20 pixels.
Display pixels(Size screen = {20, 6}) { return Display{.screen = screen, .graphics = Graphics::kitty, .cell = {10, 20}}; }

// A webview of `browser` at `address` on a scene as `given`, with what it told.
struct Surfed {
    Scene scene;
    tal::Loop loop{scene.time()};
    Document document{scene.time()};
    std::shared_ptr<ScriptedBrowser> browser;
    Webview& view;
    std::vector<std::string> visited;
    std::vector<std::string> refused;
    tal::Hooks hooks;

    // The page `address` of `given` (a scripted browser of its own when none) on a terminal as
    // `shown`.
    Surfed(std::string address, Display shown = pixels(), std::shared_ptr<tal::docs::Browser> given = nullptr)
        : scene(shown), browser(std::make_shared<ScriptedBrowser>()), view(document.children().add<Webview>(given ? given : browser, address)) {
        hooks.add(view.on<tal::Visited>([this](tal::Visited& one) { visited.push_back(one.title + "@" + one.address); }));
        hooks.add(view.on<tal::Refused>([this](tal::Refused& one) { refused.push_back(one.refusal.address); }));
        scene.show(document);
        view.focus();
        settled();
    }

    // Lets the view's work come back and be drawn.
    ~Surfed() { UNTIL(loop, !view.busy()); }

    Surfed(const Surfed&) = delete;
    Surfed& operator=(const Surfed&) = delete;

    // Lets everything asked come back and be drawn.
    void settled() {
        for (int round = 0; round < 20; ++round) {
            scene.wait(0ms);
            UNTIL(loop, !view.busy());
            QUIETED(loop);
        }
    }

    // What the browser heard, joined by " | ", then forgotten.
    std::string heard() { return joined(std::exchange(browser->heard, {})); }

    // `all` joined by " | ".
    static std::string joined(const std::vector<std::string>& all) {
        std::string made;

        for (const std::string& one : all) made += (made.empty() ? "" : " | ") + one;

        return made;
    }
};

// A local page of `html` in a file of the trials' own, as a file address.
std::string page_of(const std::string& name, const std::string& html) {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / ("tal-surfing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    std::filesystem::create_directories(folder);

    const std::filesystem::path where = folder / name;

    std::ofstream{where, std::ios::binary} << html;

#ifdef _WIN32
    return "file:///" + where.generic_string();
#else
    return tal::locating::located(tal::Path{where.string()});
#endif
}

}

TRIAL("Webview: the page opened at the room's pixels; clicks, the wheel, keys and text reach it, Ctrl stays the program's") {
    Surfed surfed{"file:///a.html"};

    SAME(surfed.browser->addresses, std::vector<std::string>{"file:///a.html"});
    OUGHT(surfed.view.busy() == false);

    surfed.heard();
    surfed.scene.click(Spot{3, 2});
    surfed.settled();
    SAME(surfed.heard(), std::string{"click 35 50"});

    surfed.scene.wheel(Spot{3, 2}, 1);
    surfed.settled();
    SAME(surfed.heard(), std::string{"wheel 0 -60"});

    surfed.scene.press(Key{U'a'});
    surfed.scene.press(Key{Code::enter});
    surfed.scene.press(Key{U'c', Mods{.ctrl = true}});
    surfed.scene.press(Key{Code::left, Mods{.alt = true}});
    surfed.scene.paste("za\xC5\xBC");
    surfed.settled();
    SAME(surfed.heard(), std::string{"type a | key 2 | back | type za\xC5\xBC"});

    surfed.view.zoom = 2;
    surfed.view.go("file:///b.html");
    surfed.settled();
    SAME(surfed.heard(), std::string{"zoom 1.5 | go file:///b.html"});

    surfed.scene.resize(Size{30, 8});
    surfed.settled();
    SAME(surfed.heard(), std::string{"resize 300x160"});
}

TRIAL("Webview: pictures every 250 ms while the page loads and none after; zoom held to its steps; keys of the program left to it") {
    Surfed surfed{"file:///a.html"};

    surfed.browser->changed("Loading", "file:///a.html", true);
    surfed.settled();

    const std::size_t loading = surfed.browser->shots;

    surfed.scene.wait(250ms);
    surfed.settled();
    surfed.scene.wait(250ms);
    surfed.settled();
    SAME(surfed.browser->shots, loading + 2);

    surfed.browser->changed("Loaded", "file:///a.html", false);
    surfed.settled();

    const std::size_t loaded = surfed.browser->shots;

    surfed.scene.wait(1000ms);
    surfed.settled();
    SAME(surfed.browser->shots, loaded);

    surfed.heard();
    surfed.view.zoom = 9;
    surfed.settled();
    SAME(*surfed.view.zoom, std::size_t{5});
    SAME(surfed.heard(), std::string{"zoom 4"});

    surfed.view.zoom = 6;
    surfed.settled();
    SAME(*surfed.view.zoom, std::size_t{5});

    const std::size_t before = surfed.browser->shots;

    for (int one = 0; one < 5; ++one) surfed.scene.press(Key{U'q'});

    surfed.settled();
    OUGHT(surfed.browser->shots <= before + 2);
    surfed.heard();
    surfed.scene.press(Key{Code::right, Mods{.alt = true}});
    surfed.settled();
    SAME(surfed.heard(), std::string{"forward"});

    surfed.scene.press(Key{Code::f5});
    surfed.scene.press(Key{U'r', Mods{.ctrl = true}});
    surfed.scene.press(Key{U'x', Mods{.alt = true}});
    surfed.settled();
    SAME(surfed.heard(), std::string{});
}

TRIAL("Webview: what the view tells comes as Visited and Refused; a lost view shows its trouble and the next click opens it anew") {
    Surfed surfed{"file:///a.html"};

    surfed.browser->changed("Strona", "file:///b.html", true, true);
    surfed.browser->refused(Refusal{Refusal::Reason::outside, "https://example.org/"});
    surfed.settled();
    SAME(Surfed::joined(surfed.visited), std::string{"Strona@file:///b.html"});
    SAME(Surfed::joined(surfed.refused), std::string{"https://example.org/"});
    SAME(surfed.view.title(), std::string{"Strona"});
    SAME(surfed.view.address(), std::string{"file:///b.html"});

    surfed.browser->changed("Nowy", "file:///b.html", false, true);
    surfed.settled();
    SAME(Surfed::joined(surfed.visited), std::string{"Strona@file:///b.html | Nowy@file:///b.html"});

    surfed.browser->lost();
    surfed.settled();
    OUGHT(surfed.scene.row(0).find("The page was lost") != std::string::npos);

    surfed.scene.click(Spot{1, 1});
    surfed.settled();
    SAME(surfed.browser->addresses.size(), std::size_t{2});
    SAME(surfed.browser->addresses.back(), std::string{"file:///b.html"});
}

TRIAL("Webview: a trouble of the engine in place of the page, then opened anew; with no engine or no pixels the text version of a local page") {
    {
        const auto failing = std::make_shared<ScriptedBrowser>();

        failing->failing = tal::Trouble{tal::Kind::broken, "The engine fell over."};

        Surfed broken{"file:///a.html", pixels({40, 6}), failing};

        SAME(broken.scene.row(0), std::string{"The engine fell over.                   "});

        failing->failing.reset();
        broken.scene.press(Key{U'x'});
        broken.settled();
        SAME(failing->addresses, std::vector<std::string>{"file:///a.html"});
    }

    const std::string local = page_of("surfing.html", "<title>Lokalna</title><h1>Nag\xC5\x82\xC3\xB3wek</h1><p>Tekst <a href=b.html>dalej</a>.");

    {
        Surfed plain{local, Display{.screen = {40, 6}}};

        SAME(plain.scene.row(0), std::string{"The terminal shows no pictures.         "});
        SAME(plain.scene.row(1), "# Nag\xC5\x82\xC3\xB3wek" + std::string(30, ' '));
        SAME(Surfed::joined(plain.visited), "Lokalna@" + local);
    }

    {
        const auto engineless = std::make_shared<ScriptedBrowser>();

        engineless->failing = tal::Trouble{tal::Kind::unsupported, "No engine here."};

        Surfed none{local, pixels({40, 6}), engineless};

        SAME(none.scene.row(0), "No engine here." + std::string(25, ' '));
        SAME(none.scene.row(1), "# Nag\xC5\x82\xC3\xB3wek" + std::string(30, ' '));
    }

#if !defined(TAL_WEBKITGTK) && !defined(TAL_WEBVIEW2)
    {
        Surfed none{local, pixels({40, 6}), tal::docs::Browser::builtin()};

        SAME(none.scene.row(0), std::string{"This program was built without a web    "});
        SAME(none.scene.row(1).substr(0, 7), std::string{"engine."});
    }
#endif

    {
        Surfed away{"https://example.org/", Display{.screen = {40, 6}}};

        SAME(away.scene.row(0), std::string{"The terminal shows no pictures. The page"});
    }
}
