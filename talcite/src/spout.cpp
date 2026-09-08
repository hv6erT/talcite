#include "spout.hpp"

#include <cstddef>

#ifdef _WIN32

#include <windows.h>

#else

#include <unistd.h>

#endif

namespace talcite {

void writing(std::string_view bytes) {
#ifdef _WIN32

    DWORD given = 0;

    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()),
              &given, nullptr);

#else

    std::size_t put = 0;

    while (put < bytes.size()) {
        const ssize_t given = ::write(STDOUT_FILENO, bytes.data() + put, bytes.size() - put);

        if (given <= 0) return;

        put += static_cast<std::size_t>(given);
    }

#endif
}

}
