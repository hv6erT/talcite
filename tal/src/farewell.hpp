#pragma once

#include <string_view>

namespace tal::detail {

// What gives the terminal back after a signal (SIGTERM, SIGHUP, SIGTSTP), when there is no time to
// undress it the way `Term` does: one fixed string, because a signal handler may only `write`. It
// turns off everything `Term` can turn on, in the order `Term` itself undresses: the synchronized
// frame a signal may have cut in half first, then paste and the mouse in all its encodings, the
// wrap back on, the cursor's own shape and the cursor itself, and both kinds of alternate screen
// last, so that everything before lands on the screen it was meant for. Turning off something that
// was not turned on does nothing. A trial checks that every sequence `Term` undresses with is here.
inline constexpr std::string_view farewell = "\x1b[?2026l\x1b[?2004l\x1b[?1003l\x1b[?1006l\x1b[?1015l\x1b[?1002l\x1b[?1000l"
                                             "\x1b[?7h\x1b[0 q\x1b[?25h\x1b[?1049l\x1b[?47l";

}
