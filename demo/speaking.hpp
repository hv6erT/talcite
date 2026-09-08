#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#endif

namespace showing {

class Speaking {
public:
    Speaking() {
#ifdef _WIN32
        was_ = GetConsoleOutputCP();

        SetConsoleOutputCP(CP_UTF8);
#endif
    }

    ~Speaking() {
#ifdef _WIN32
        if (was_ != 0) SetConsoleOutputCP(was_);
#endif
    }

    Speaking(const Speaking&) = delete;
    Speaking& operator=(const Speaking&) = delete;
    Speaking(Speaking&&) = delete;
    Speaking& operator=(Speaking&&) = delete;

#ifdef _WIN32
private:
    UINT was_ = 0;
#endif
};

}
