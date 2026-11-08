#include <stdexcept>

#include "core.hpp"
#include "offload.hpp"

#ifdef _WIN32

#include <windows.h>

#else

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#endif

// The bell of a thread of the io pool, ported from talcite's bell.cpp: what a call that blocks in
// the system waits for together with its own descriptor or handle, so that cancelling the task
// wakes it at once (`detail::Chain::heed`). One for each thread, made on its first use and
// silenced before every piece of work.

namespace tal::platform {

#ifdef _WIN32

// A manual-reset event that starts silent; Windows refusing it is a broken system.
Bell::Bell() {
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (event_ == nullptr) throw std::runtime_error("tal::platform::Bell: cannot create the event");
}

// Closes the event.
Bell::~Bell() {
    if (event_ != nullptr) CloseHandle(event_);
}

// Rings the bell: the event stays signaled until `clear`.
void Bell::woken() {
    rang_.store(true);
    SetEvent(event_);
}

// Silences the bell.
void Bell::clear() {
    rang_.store(false);
    ResetEvent(event_);
}

#else

// A pipe, so that a wait can take the bell together with its own descriptor in one poll(); it does
// not leak into child programs.
Bell::Bell() {
    int both[2] = {-1, -1};

    if (::pipe2(both, O_CLOEXEC | O_NONBLOCK) != 0) throw std::runtime_error("tal::platform::Bell: cannot create the pipe");

    heard_ = both[0];
    rung_ = both[1];
}

// Closes both ends of the pipe.
Bell::~Bell() {
    if (heard_ >= 0) ::close(heard_);
    if (rung_ >= 0) ::close(rung_);
}

// Rings the bell by writing one byte into the pipe; a full pipe rings already.
void Bell::woken() {
    rang_.store(true);

    const unsigned char one = 1;
    const ssize_t wrote = ::write(rung_, &one, 1);

    (void)wrote;
}

// Silences the bell by draining the pipe; a read interrupted by a signal is retried, since a byte
// left behind would wake the next work's waits at once.
void Bell::clear() {
    rang_.store(false);

    unsigned char room[64];

    for (;;) {
        const ssize_t got = ::read(heard_, room, sizeof(room));

        if (got > 0 || (got < 0 && errno == EINTR)) continue;

        break;
    }
}

#endif

// Whether the bell has rung since it was last silenced.
bool Bell::rang() const { return rang_.load(); }

namespace core {

namespace {

// The bell of this thread, made on its first use.
thread_local std::unique_ptr<Bell> own;

// The bell of the work running on this thread now.
thread_local Bell* current = nullptr;

}

// The bell of this thread, silenced for a new piece of work.
Bell* bell() {
    if (!own) own = std::make_unique<Bell>();

    own->clear();

    return own.get();
}

// The bell and the cancellation flag of the work running on this thread now, or none afterwards.
void belled(Bell* one, const std::atomic<bool>* flag) {
    current = one;
    halted(flag);
}

// The bell of the work running on this thread, for a call that blocks.
Bell* ringing() { return current; }

}

}
