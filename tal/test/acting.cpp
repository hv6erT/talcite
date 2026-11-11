#include <optional>
#include <string>
#include <vector>

#include "tal/action.hpp"
#include "tal/event.hpp"
#include "tal/key.hpp"

#include "tal/testing.hpp"

// Actions: one source of a command's text, letter, shortcut and availability, which its views
// hear about, and which runs its handlers in order.

namespace {

using tal::Action;
using tal::Altered;
using tal::Code;
using tal::Hook;
using tal::Hooks;
using tal::Key;
using tal::Run;

TRIAL("Action: the text, letter, shortcut and availability it was made with") {
    const Action plain{"Copy"};
    const Action full{"Copy", U'C', Key{Code::f5}};

    SAME(plain.text(), std::string{"Copy"});
    OUGHT(!plain.letter().has_value());
    OUGHT(!plain.key().has_value());
    OUGHT(plain.enabled());
    OUGHT(full.letter() == std::optional<char32_t>{U'C'});
    OUGHT(full.key() == std::optional<Key>{Key{Code::f5}});
}

// Every change the views show is told once; setting what already stands tells nothing.
TRIAL("Action: a change of what it shows is told to its views, once each") {
    Action one{"Copy"};
    std::vector<std::string> heard;
    Hooks hooks;

    hooks.add(one.on<Altered>([&heard](Altered& what) {
        heard.push_back(what.action.text() + (what.action.enabled() ? "+" : "-") +
                        (what.action.letter() ? std::string(1, static_cast<char>(*what.action.letter())) : std::string{}) +
                        (what.action.key() ? "k" : ""));
    }));

    one.text("Copy");
    one.letter(std::nullopt);
    one.key(std::nullopt);
    one.enable(true);

    OUGHT(heard.empty());

    one.text("Move");
    one.letter(U'M');
    one.key(Key{Code::f6});
    one.enable(false);
    one.enable(false);
    one.key(Key{Code::f6});

    OUGHT(heard == (std::vector<std::string>{"Move+", "Move+M", "Move+Mk", "Move-Mk"}));
}

// Running goes to the handlers in the order they were added, until one takes it; a disabled action
// does nothing, and a handler whose hook went is not called.
TRIAL("Action: running calls its handlers in order until one takes it, and a disabled one does nothing") {
    Action one{"Delete"};
    std::vector<int> ran;
    Hook first = one.on<Run>([&ran](Run&) { ran.push_back(1); });
    Hook second = one.on<Run>([&ran](Run& what) {
        ran.push_back(2);
        what.take();
    });
    Hook third = one.on<Run>([&ran](Run&) { ran.push_back(3); });

    one.run();

    OUGHT(ran == (std::vector<int>{1, 2}));

    second = Hook{};
    ran.clear();

    one.run();

    OUGHT(ran == (std::vector<int>{1, 3}));

    one.enable(false);
    ran.clear();

    one.run();

    OUGHT(ran.empty());
}

// A handler may add and remove handlers while it runs: the ones it adds wait for the next run, the
// ones it removes are skipped at once.
TRIAL("Action: handlers added during a run wait for the next one, removed ones are skipped") {
    Action one{"Edit"};
    std::vector<int> ran;
    Hooks later;
    Hook removed;
    Hook first = one.on<Run>([&](Run&) {
        ran.push_back(1);
        removed = Hook{};
        later.add(one.on<Run>([&ran](Run&) { ran.push_back(4); }));
    });

    removed = one.on<Run>([&ran](Run&) { ran.push_back(2); });

    one.run();

    OUGHT(ran == (std::vector<int>{1}));

    ran.clear();
    first = Hook{};

    one.run();

    OUGHT(ran == (std::vector<int>{4}));
}

}
