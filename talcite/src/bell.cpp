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

Bell::Bell() {
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event_ == nullptr) throw std::runtime_error("talcite::Bell: cannot create the event");
}

Bell::~Bell() {
    if (event_ != nullptr) CloseHandle(event_);
}

void Bell::ring() { SetEvent(event_); }

void Bell::clear() { ResetEvent(event_); }

bool Bell::wait(std::chrono::milliseconds patience) {
    const DWORD how = WaitForSingleObject(
        event_, patience.count() < 0 ? INFINITE : static_cast<DWORD>(patience.count()));

    return how == WAIT_OBJECT_0;
}

#else

// Rurka, a nie zmienna warunkowa: pętla musi umieć czekać na dzwonek razem z klawiaturą i
// gniazdami, w jednym poll(), a do tego potrzebny jest deskryptor.
Bell::Bell() {
    int both[2] = {-1, -1};

    // Rurki dzwonka nie mają trafiać do programu potomnego — pula ma ich dwie na robotnika.
    if (!sealed::piped(both, true)) throw std::runtime_error("talcite::Bell: cannot create the pipe");

    heard_ = both[0];
    rung_ = both[1];
}

Bell::~Bell() {
    if (heard_ >= 0) ::close(heard_);
    if (rung_ >= 0) ::close(rung_);
}

void Bell::ring() {
    const unsigned char one = 1;
    const ssize_t wrote = ::write(rung_, &one, 1);
    (void)wrote;
}

// **Wznawiamy po przerwaniu sygnałem.** Zakończenie pętli z bajtem w rurce sprawia, że następne
// zadanie w puli widzi dzwonek i uznaje się za anulowane, a wtedy obietnica nie rozstrzyga się
// wcale i `await` czeka bez końca.
void Bell::clear() {
    unsigned char room[64];

    for (;;) {
        const ssize_t got = ::read(heard_, room, sizeof(room));

        if (got > 0) continue;

        if (got < 0 && errno == EINTR) continue;

        break;
    }
}

bool Bell::wait(std::chrono::milliseconds patience) {
    pollfd watched{};
    watched.fd = heard_;
    watched.events = POLLIN;

    const int how = ::poll(&watched, 1, static_cast<int>(patience.count()));

    return how > 0;
}

#endif

}
