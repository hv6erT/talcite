#include "netting.hpp"

#include <chrono>
#include <string>

#include "proving.hpp"

using namespace talcite;

// The pieces every networking file shares: the patience left until a deadline, a promise refused
// with a text, and the address a closed endpoint reports.

TRIAL("net: left rounds the time up to a whole millisecond, never down") {
    using namespace std::chrono_literals;

    const auto before = std::chrono::steady_clock::now();
    const auto until = before + 100ms + 500us;
    const net::Patience rest = net::left(until);
    const auto after = std::chrono::steady_clock::now();

    ought(rest >= until - after);
    ought(rest < until - before + 1ms);
}

TRIAL("net: left gives a deadline already passed one millisecond, not zero") {
    using namespace std::chrono_literals;

    const auto now = std::chrono::steady_clock::now();

    same(net::left(now - 1s).count(), 1);
    ought(net::left(now).count() >= 1);
}

TRIAL("net: refused is a promise failed with Trouble and the text given") {
    Promise<int> one = net::refused<int>("kanał zamknięty");

    std::string said;

    try {
        (void)std::move(one).get();
    } catch (const net::Trouble& why) {
        said = why.what();
    }

    same(said, std::string{"kanał zamknięty"});
}

TRIAL("net: nowhere is one empty address") {
    ought(!net::nowhere().held());
    ought(&net::nowhere() == &net::nowhere());
}
