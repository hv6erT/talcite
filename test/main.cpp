#include "talcite/root.hpp"

#include "tal/testing.hpp"

// Runs the trials of talcite on the runner of `tal::testing`. Every frame of a root is painted
// whole as well and compared with painting only the changes (`Root::frame`), so a change nobody
// announced fails the trial that made it.
int main(int count, char** given) {
    talcite::ui::frames_checked(true);

    return tal::testing::run(count, given);
}
