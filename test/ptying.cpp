#include "proving.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "talcite/child.hpp"
#include "talcite/fs.hpp"
#include "talcite/pty.hpp"

using namespace talcite;

namespace {

// The test binary pretending to be a program in a terminal (`tty` in proving.cpp).
Command typing() {
    Command made;

    made.program = fs::path{proving::mine()};
    made.arguments = {"--pretending", "tty"};

    return made;
}

// Reads what the terminal shows until `wanted` is among it, five seconds at most per read, and
// gives everything read; what was read so far comes in `seen`. The end of the terminal ends the
// reading too.
bool shown(const Pty& one, const std::string& wanted, std::string& seen) {
    for (int turn = 0; turn < 200 && seen.find(wanted) == std::string::npos; ++turn) {
        const Bytes got = one.read(4096).timeout(std::chrono::milliseconds{5000}).get();

        if (got.empty()) break;

        seen += text(got);
    }

    return seen.find(wanted) != std::string::npos;
}

}

// Every POSIX system has pseudoterminals; Windows from 10 1809 on, which every Windows the tests
// run on has.
TRIAL("Pty: the system has pseudoterminals") { ought(Pty::supported()); }

// A program started in a pseudoterminal has a terminal for its input and output, of the size it
// was given, and ends with its own code when asked.
TRIAL("Pty: a program sees a terminal of the given size") {
    const Pty one = Pty::start(typing(), {100, 30}).get();

    ought(one.held());
    ought(one.pid() > 0);

    std::string seen;

    ought(shown(one, "tty 1 100x30", seen));

    (void)one.write(bytes("quit\r")).get();

    const Exit end = one.wait().timeout(std::chrono::milliseconds{5000}).get();

    ought(end.ended == Ended::normal);
    same(end.code, 0);
}

// What is written arrives as typed keys: the terminal echoes it, and the program gets the line once
// Enter (a carriage return, as a keyboard sends) ends it.
TRIAL("Pty: what is written arrives as typed keys") {
    const Pty one = Pty::start(typing(), {80, 24}).get();

    std::string seen;

    ought(shown(one, "tty 1 80x24", seen));

    (void)one.write(bytes("dzień dobry\r")).get();

    ought(shown(one, "got:dzień dobry", seen));

    (void)one.write(bytes("quit\r")).get();
    (void)one.wait().timeout(std::chrono::milliseconds{5000}).get();
}

// A new size reaches the program at once: asked again, it gives the new one.
TRIAL("Pty: a new size reaches the program") {
    const Pty one = Pty::start(typing(), {80, 24}).get();

    std::string seen;

    ought(shown(one, "tty 1 80x24", seen));

    one.resize({60, 20});

    (void)one.write(bytes("size\r")).get();

    ought(shown(one, "size 60x20", seen));

    (void)one.write(bytes("quit\r")).get();
    (void)one.wait().timeout(std::chrono::milliseconds{5000}).get();
}

// The end of the program is the end of the terminal: a read after it gives nothing more, as at the
// end of a pipe.
TRIAL("Pty: the stream ends with the program") {
    const Pty one = Pty::start(typing(), {80, 24}).get();

    std::string seen;

    ought(shown(one, "tty 1", seen));

    (void)one.write(bytes("quit\r")).get();
    (void)one.wait().timeout(std::chrono::milliseconds{5000}).get();

    bool ended = false;

    for (int turn = 0; turn < 200 && !ended; ++turn) ended = one.read(4096).timeout(std::chrono::milliseconds{5000}).get().empty();

    ought(ended);
}

// Hanging up ends a program waiting for a line, as closing a terminal window ends the shell in it.
TRIAL("Pty: hanging up ends a program that waits for input") {
    const Pty one = Pty::start(typing(), {80, 24}).get();

    std::string seen;

    ought(shown(one, "tty 1", seen));

    one.hang_up();

    const Exit end = one.wait().timeout(std::chrono::milliseconds{5000}).get();

    ought(end.ended != Ended::lost);
}

// An empty handle knows nothing and does nothing.
TRIAL("Pty: an empty handle knows nothing") {
    const Pty one;

    ought(!one.held());
    same(one.pid(), std::int64_t{-1});

    one.resize({10, 10});
    one.hang_up();
}
