#include "remoting.hpp"

namespace talcite {

// Whether the program runs over ssh. Then this machine is not the one the user sits at: its
// clipboard and its browser are someone else's, and only the terminal reaches the user's. The
// environment is read from the console, because the console knows where the user sits, and thanks
// to that a test can pretend to be ssh on any system. The clipboard and the launcher ask the same
// question, so it is answered in one place.
bool remote(const Console& from) {
    return from.variable("SSH_CONNECTION").has_value() || from.variable("SSH_TTY").has_value();
}

}
