#include "talcite/bell.hpp"

#include "sealed.hpp"

#include <stdexcept>

#ifdef _WIN32

#include <windows.h>

#else

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#endif

namespace talcite {

#ifdef _WIN32

// A manual-reset event that starts silent. Throws when Windows cannot create it.
Bell::Bell() {
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event_ == nullptr) throw std::runtime_error("talcite::Bell: cannot create the event");
}

// Closes the event.
Bell::~Bell() {
    if (event_ != nullptr) CloseHandle(event_);
}

// Rings the bell: the event stays signaled until `clear`.
void Bell::ring() {
    rang_.store(true);
    SetEvent(event_);
}

// Silences the bell.
void Bell::clear() {
    rang_.store(false);
    ResetEvent(event_);
}

// Waits for the bell at most `patience`, or without limit when it is negative. Returns whether the
// bell rang.
bool Bell::wait(std::chrono::milliseconds patience) {
    const DWORD how = WaitForSingleObject(
        event_, patience.count() < 0 ? INFINITE : static_cast<DWORD>(patience.count()));

    return how == WAIT_OBJECT_0;
}

#else

// A pipe, not a condition variable: the loop has to be able to wait for the bell together with the
// keyboard and sockets, in one poll(), and that takes a descriptor. Throws when the pipe cannot be
// created.
Bell::Bell() {
    int both[2] = {-1, -1};

    // The bell's pipe must not leak into a child program; the pool has two of them per worker.
    if (!sealed::piped(both, true)) throw std::runtime_error("talcite::Bell: cannot create the pipe");

    heard_ = both[0];
    rung_ = both[1];
}

// Closes both ends of the pipe.
Bell::~Bell() {
    if (heard_ >= 0) ::close(heard_);
    if (rung_ >= 0) ::close(rung_);
}

// Rings the bell by writing one byte into the pipe. A full pipe already rings, so a failed write is
// ignored.
void Bell::ring() {
    rang_.store(true);

    const unsigned char one = 1;
    const ssize_t wrote = ::write(rung_, &one, 1);
    (void)wrote;
}

// Silences the bell by draining the pipe. A read interrupted by a signal is retried: leaving the
// loop with a byte still in the pipe would wake the next task's waits on the bell at once, as
// though its work had been stopped.
void Bell::clear() {
    rang_.store(false);

    unsigned char room[64];

    for (;;) {
        const ssize_t got = ::read(heard_, room, sizeof(room));

        if (got > 0) continue;

        if (got < 0 && errno == EINTR) continue;

        break;
    }
}

// Waits for the bell at most `patience`, or without limit when it is negative. Returns whether the
// bell rang.
bool Bell::wait(std::chrono::milliseconds patience) {
    pollfd watched{};
    watched.fd = heard_;
    watched.events = POLLIN;

    const int how = ::poll(&watched, 1, static_cast<int>(patience.count()));

    return how > 0;
}

#endif

// Whether the bell has rung since the last `clear`, without waiting and without silencing it.
bool Bell::rang() const { return rang_.load(); }

}
