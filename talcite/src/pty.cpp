#include "talcite/pty.hpp"

#include <cstddef>
#include <utility>

#include "talcite/pool.hpp"

#include "teletype.hpp"

// A program in a pseudoterminal: what a terminal window gives a shell. Its input and output are one
// stream each way, raw bytes with the terminal's escape sequences, and the line editing, echo and
// signals of the keyboard are the terminal's business. The program and its handle are a `Child`
// started in a terminal (`teletype` in child.cpp), so the burial, the reading and the writing on
// the pool are the same as for a program on pipes.
namespace talcite {

// Whether pseudoterminals exist here: on every POSIX system, on Windows from 10 1809 on. Where they
// do not, `start` refuses, and the caller can say so before trying.
bool Pty::supported() { return teletyped(); }

// Starts the program in a new pseudoterminal of the given size (columns and rows), on the pool.
// Refuses with `fs::Trouble` when it cannot be started or the system has no pseudoterminals.
Promise<Pty> Pty::start(Command what, ui::Size size) {
    return talcite::run(io(), [what = std::move(what), size]() -> Pty { return Pty{teletype(what, size)}; });
}

// Writes the bytes to the terminal, as typed keys, on the pool; resolves to how many went.
Promise<std::size_t> Pty::write(Bytes what) const { return child_.write(std::move(what)); }

// Reads at most `most` bytes of what the program shows, on the pool; empty once the terminal is
// over (the program and whatever it started have closed it).
Promise<Bytes> Pty::read(std::size_t most) const { return child_.read(most); }

// The new size of the terminal, in columns and rows; the program hears of it at once.
void Pty::resize(ui::Size size) const { resized(child_, size); }

// The program's exit; see `Child::wait`.
Promise<Exit> Pty::wait() const { return child_.wait(); }

// Hangs up the terminal, as closing a terminal window does; a shell then ends with what it runs.
void Pty::hang_up() const { hung_up(child_); }

// Kills the program and its group at once; see `Child::kill`.
void Pty::kill() const { child_.kill(); }

// Lets go of the handle; the program goes on, as with `Child::close`.
void Pty::close() { child_.close(); }

}
