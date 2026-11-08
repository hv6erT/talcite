#include "watch.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32

#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#endif

#if defined(__linux__)

#include <cerrno>
#include <csignal>
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <sys/syscall.h>
#include <unistd.h>

#endif

// The watch over the running trial. A trial that hangs (a deadlock, a wait without a deadline)
// would otherwise hold the run until the outer timeout of the verify script, and the log would not
// say which trial it was. The watch names the trial, writes the stacks of the other threads (on
// Windows x64 and x86 and on Linux), and ends the program at once.

namespace tal::testing::detail {

namespace {

// How long one trial may run before the watch calls it hung: `TAL_TRIAL_LIMIT` in seconds, five
// minutes without it. The slowest honest trial takes a few seconds, a few tens under a sanitizer,
// so five minutes leaves a wide margin; the trial of the watch itself sets one second.
std::chrono::seconds longest() {
    const char* given = std::getenv("TAL_TRIAL_LIMIT");
    const int seconds = given != nullptr ? std::atoi(given) : 0;

    return std::chrono::seconds{seconds > 0 ? seconds : 300};
}

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__))

// One loaded module of the program: where it lies and its name.
struct Loaded {
    DWORD64 base = 0;
    DWORD64 size = 0;
    std::string name;
};

// The modules of the program, read from the process's own list (psapi), which takes no lock of
// the loader: a hang at the end of a run may hold that lock.
std::vector<Loaded> loaded() {
    const HANDLE self = GetCurrentProcess();

    std::vector<HMODULE> handles(1024);
    DWORD needed = 0;

    if (!K32EnumProcessModules(self, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed)) return {};

    handles.resize(std::min<std::size_t>(handles.size(), needed / sizeof(HMODULE)));

    std::vector<Loaded> made;

    for (const HMODULE one : handles) {
        MODULEINFO about{};
        char name[MAX_PATH] = {};

        if (!K32GetModuleInformation(self, one, &about, sizeof(about))) continue;

        (void)K32GetModuleBaseNameA(self, one, name, MAX_PATH);

        made.push_back(Loaded{static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(about.lpBaseOfDll)), about.SizeOfImage, name});
    }

    return made;
}

// An address as module and offset, or bare when no module holds it.
std::string placed(const std::vector<Loaded>& modules, DWORD64 at) {
    char made[MAX_PATH + 32] = {};

    for (const Loaded& one : modules)
        if (at >= one.base && at < one.base + one.size) {
            std::snprintf(made, sizeof(made), "%s+0x%llx", one.name.c_str(), static_cast<unsigned long long>(at - one.base));

            return made;
        }

    std::snprintf(made, sizeof(made), "0x%llx", static_cast<unsigned long long>(at));

    return made;
}

// Reads `size` bytes of the program's own memory at `at`; false where nothing is mapped there.
bool fetched(DWORD64 at, void* into, std::size_t size) {
    SIZE_T got = 0;

    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(at)), into, size, &got) &&
           got == size;
}

#if defined(_M_IX86) || defined(__i386__)

// The return addresses of a stopped thread, at most 32, along the chain of frame pointers (EBP):
// 32-bit code has no tables to unwind by. The system's libraries keep the chain; the program's own
// optimized code may not, and then the chain skips or ends early, so `scanned` adds what the stack
// holds.
std::vector<DWORD64> unwound(const CONTEXT& context) {
    std::vector<DWORD64> made{context.Eip};
    DWORD64 frame = context.Ebp;

    for (int depth = 1; depth < 32 && frame != 0; ++depth) {
        DWORD pair[2] = {};

        if (!fetched(frame, pair, sizeof(pair)) || pair[1] == 0) break;

        made.push_back(pair[1]);

        if (pair[0] <= frame) break;

        frame = pair[0];
    }

    return made;
}

// Whether the code just before `at` is a call (E8 with four bytes; FF /2 with two, three, six or
// seven bytes), so that `at` may be where one returns.
bool called(DWORD64 at) {
    unsigned char before[7] = {};

    if (at < sizeof(before) || !fetched(at - sizeof(before), before, sizeof(before))) return false;

    const auto indirect = [&](std::size_t op) { return before[op] == 0xFF && (before[op + 1] & 0x38) == 0x10; };

    return before[2] == 0xE8 || indirect(0) || indirect(1) || indirect(4) || indirect(5);
}

// The words on the top 8 KiB of a stopped thread's stack that point into a loaded module just
// after a call, at most 40: the return addresses among them, with some left over from earlier
// calls. It needs no tables and no frame pointers, so it holds where the chain does not.
std::vector<DWORD64> scanned(const CONTEXT& context, const std::vector<Loaded>& modules) {
    std::vector<DWORD64> made;

    for (DWORD64 at = context.Esp; at < context.Esp + 8192 && made.size() < 40; at += sizeof(DWORD)) {
        DWORD word = 0;

        if (!fetched(at, &word, sizeof(word))) break;

        const bool inside = std::any_of(modules.begin(), modules.end(),
                                        [&](const Loaded& one) { return word >= one.base && word < one.base + one.size; });

        if (inside && called(word)) made.push_back(word);
    }

    return made;
}

#else

// The return addresses of a stopped thread, at most 32, unwound by the tables of the code itself
// (RtlLookupFunctionEntry, RtlVirtualUnwind), as the system unwinds an exception, with no help of
// dbghelp: a leaf function without a table returns through the address on top of its stack.
std::vector<DWORD64> unwound(CONTEXT context) {
    std::vector<DWORD64> made;

    for (int depth = 0; depth < 32 && context.Rip != 0; ++depth) {
        made.push_back(context.Rip);

        DWORD64 image = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);

        if (function == nullptr) {
            DWORD64 back = 0;

            if (!fetched(context.Rsp, &back, sizeof(back))) break;

            context.Rip = back;
            context.Rsp += sizeof(back);

            continue;
        }

        PVOID handler = nullptr;
        DWORD64 establisher = 0;

        (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler, &establisher, nullptr);
    }

    return made;
}

// On 64-bit Windows the tables give the whole stack, and nothing needs scanning.
std::vector<DWORD64> scanned(const CONTEXT&, const std::vector<Loaded>&) {
    return {};
}

#endif

// The stacks the watch wrote: every other thread with its return addresses, the words that look
// like ones (`scanned`, 32-bit only), and its registers, from which dbghelp walks it again.
struct Told {
    DWORD id = 0;
    std::vector<DWORD64> frames;
    std::vector<DWORD64> found;
    CONTEXT context{};
};

// Writes every other thread of the program with its stack as module and offset, at once, and
// stops the threads for it; they stay stopped, as the program ends right after. Then it writes the
// same frames with the names of functions through dbghelp, which may itself wait for the loader's
// lock, and so comes second (`threads_named`).
std::vector<Told> threads_told() {
    const DWORD process = GetCurrentProcessId();
    const DWORD mine = GetCurrentThreadId();
    const std::vector<Loaded> modules = loaded();

    std::vector<Told> made;

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

    if (snapshot == INVALID_HANDLE_VALUE) return made;

    THREADENTRY32 entry{};

    entry.dwSize = sizeof(entry);

    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == mine) continue;

        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);

        if (thread == nullptr) continue;

        (void)SuspendThread(thread);

        CONTEXT context{};

        context.ContextFlags = CONTEXT_FULL;

        Told one{entry.th32ThreadID, {}, {}, {}};

        if (GetThreadContext(thread, &context)) {
            one.frames = unwound(context);
            one.found = scanned(context, modules);
            one.context = context;
        }

        std::fprintf(stdout, "        thread %lu\n", static_cast<unsigned long>(one.id));

        for (const DWORD64 at : one.frames) std::fprintf(stdout, "          %s\n", placed(modules, at).c_str());

        if (!one.found.empty()) std::fprintf(stdout, "          found on the stack:\n");

        for (const DWORD64 at : one.found) std::fprintf(stdout, "          %s\n", placed(modules, at).c_str());

        std::fflush(stdout);
        CloseHandle(thread);

        made.push_back(std::move(one));
    }

    CloseHandle(snapshot);

    return made;
}

// One address as module, function and offset where dbghelp knows the function, written as a line.
void name_told(HANDLE self, DWORD64 at) {
    alignas(SYMBOL_INFO) char room[sizeof(SYMBOL_INFO) + 256] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(room);

    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;

    IMAGEHLP_MODULE64 module{};

    module.SizeOfStruct = sizeof(module);

    const char* name = SymGetModuleInfo64(self, at, &module) ? module.ModuleName : "?";
    DWORD64 offset = 0;

    if (SymFromAddr(self, at, &offset, symbol))
        std::fprintf(stdout, "          %s!%s+0x%llx\n", name, symbol->Name, static_cast<unsigned long long>(offset));
    else
        std::fprintf(stdout, "          %s ?\n", name);
}

#if defined(_M_IX86) || defined(__i386__)

// The stack of a stopped 32-bit thread walked by dbghelp (StackWalk64), at most 48 frames: with the
// symbol file it knows the frames without a frame pointer, which the chain in `unwound` skips.
std::vector<DWORD64> walked(HANDLE self, const Told& one) {
    std::vector<DWORD64> made;
    const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, one.id);

    if (thread == nullptr) return made;

    CONTEXT context = one.context;
    STACKFRAME64 frame{};

    frame.AddrPC.Offset = context.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Esp;
    frame.AddrStack.Mode = AddrModeFlat;

    while (made.size() < 48 && StackWalk64(IMAGE_FILE_MACHINE_I386, self, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                                           SymGetModuleBase64, nullptr) &&
           frame.AddrPC.Offset != 0)
        made.push_back(frame.AddrPC.Offset);

    CloseHandle(thread);

    return made;
}

#endif

// The frames written before, now with the names of functions where dbghelp knows them (exports of
// the system's libraries, and the program's own with its symbol file). A 32-bit stack is walked
// again by dbghelp, and the words found on it are named after the walk.
void threads_named(const std::vector<Told>& all) {
    const HANDLE self = GetCurrentProcess();

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);

    if (!SymInitialize(self, nullptr, TRUE)) return;

    std::fprintf(stdout, "        the same, named:\n");

    for (const Told& one : all) {
        std::fprintf(stdout, "        thread %lu\n", static_cast<unsigned long>(one.id));

#if defined(_M_IX86) || defined(__i386__)
        const std::vector<DWORD64> frames = one.frames.empty() ? one.frames : walked(self, one);
#else
        const std::vector<DWORD64>& frames = one.frames;
#endif

        for (const DWORD64 at : frames) name_told(self, at);

        if (!one.found.empty()) std::fprintf(stdout, "          found on the stack:\n");

        for (const DWORD64 at : one.found) name_told(self, at);

        std::fflush(stdout);
    }
}

// The thread that writes the stacks, bare and then named, so that the watch can stop waiting for
// it.
DWORD WINAPI told_apart(LPVOID) {
    const std::vector<Told> all = threads_told();

    threads_named(all);

    return 0;
}

#endif
#if defined(__linux__)

// The signal that asks a thread for its stack: one of the real-time ones, which nothing else in
// the trials uses.
int asking() { return SIGRTMIN + 4; }

// The thread asked for its stack now: its number while the request is open, minus its number
// while it writes the stack, zero when it is done or nobody asks.
std::atomic<long> asked{0};

// The stack the asked thread wrote: its return addresses and how many there are.
void* caught[48];
std::atomic<int> caught_much{0};

// The number of the calling thread in the kernel.
long thread_number() { return static_cast<long>(::syscall(SYS_gettid)); }

// Answers a request for the stack, in the thread asked: it claims the request, so that a late
// answer to a request already given up writes nothing, and takes only the return addresses
// (`backtrace` was called once before, so it loads nothing here). The watch writes them out.
void answered(int) {
    const int kept = errno;
    long expected = thread_number();

    if (asked.compare_exchange_strong(expected, -expected)) {
        caught_much.store(::backtrace(caught, 48), std::memory_order_relaxed);
        asked.store(0, std::memory_order_release);
    }

    errno = kept;
}

// One stretch of a file mapped as code: where it lies in memory and where the file starts in
// memory, so that an address becomes the file's name and an offset `addr2line` takes.
struct Loaded {
    std::uintptr_t from = 0;
    std::uintptr_t to = 0;
    std::uintptr_t base = 0;
    std::string name;
};

// The code mapped into the program, read from `/proc/self/maps` with plain reads: nothing that
// takes a lock of the loader, which a hang may hold.
std::vector<Loaded> loaded() {
    std::string read;
    const int file = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);

    if (file < 0) return {};

    char room[4096];

    for (ssize_t got = ::read(file, room, sizeof room); got > 0; got = ::read(file, room, sizeof room))
        read.append(room, static_cast<std::size_t>(got));

    ::close(file);

    std::vector<Loaded> made;
    std::map<std::string, std::uintptr_t> bases;
    std::size_t at = 0;

    while (at < read.size()) {
        const std::size_t end = std::min(read.find('\n', at), read.size());
        const std::string line = read.substr(at, end - at);

        at = end + 1;

        unsigned long long from = 0;
        unsigned long long to = 0;
        unsigned long long offset = 0;
        char rights[8] = {};
        int taken = 0;

        if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s%n", &from, &to, rights, &offset, &taken) < 4) continue;

        const std::size_t named = line.find('/', static_cast<std::size_t>(taken));

        if (named == std::string::npos) continue;

        const std::string name = line.substr(named);

        if (offset == 0 && !bases.contains(name)) bases[name] = static_cast<std::uintptr_t>(from);

        if (rights[2] != 'x') continue;

        made.push_back(Loaded{static_cast<std::uintptr_t>(from), static_cast<std::uintptr_t>(to), 0, name});
    }

    for (Loaded& one : made) one.base = bases.contains(one.name) ? bases[one.name] : one.from;

    return made;
}

// An address as file and offset, or bare when no mapped file holds it.
std::string placed(const std::vector<Loaded>& modules, std::uintptr_t at) {
    char made[64] = {};

    for (const Loaded& one : modules)
        if (at >= one.from && at < one.to) {
            std::snprintf(made, sizeof made, "+0x%llx", static_cast<unsigned long long>(at - one.base));

            const std::size_t slash = one.name.find_last_of('/');

            return one.name.substr(slash + 1) + made;
        }

    std::snprintf(made, sizeof made, "0x%llx", static_cast<unsigned long long>(at));

    return made;
}

// Whether the thread blocks the signal that asks for its stack (a sanitizer's own thread does), so
// that it is not waited for: read from its `SigBlk` in `/proc/self/task/<id>/status`.
bool blocks_asking(long id) {
    const std::string named = "/proc/self/task/" + std::to_string(id) + "/status";
    const int file = ::open(named.c_str(), O_RDONLY | O_CLOEXEC);

    if (file < 0) return false;

    std::string read;
    char room[1024];

    for (ssize_t got = ::read(file, room, sizeof room); got > 0; got = ::read(file, room, sizeof room))
        read.append(room, static_cast<std::size_t>(got));

    ::close(file);

    const std::size_t at = read.find("SigBlk:");

    if (at == std::string::npos) return false;

    const unsigned long long mask = std::strtoull(read.c_str() + at + 7, nullptr, 16);

    return ((mask >> (asking() - 1)) & 1U) != 0;
}

// One thread and the return addresses it wrote.
struct Told {
    long id = 0;
    std::vector<std::uintptr_t> frames;
};

// Asks every other thread of the program for its stack, one at a time, and writes each as file and
// offset at once. A thread that does not answer within two seconds (it blocks the signal, or it
// hangs inside the kernel) is named without a stack.
std::vector<Told> threads_told() {
    const long mine = thread_number();
    const std::vector<Loaded> modules = loaded();
    std::vector<long> numbers;

    if (DIR* listed = ::opendir("/proc/self/task"); listed != nullptr) {
        while (const dirent* one = ::readdir(listed))
            if (one->d_name[0] != '.' && std::atol(one->d_name) != mine) numbers.push_back(std::atol(one->d_name));

        ::closedir(listed);
    }

    std::vector<Told> made;

    for (const long id : numbers) {
        std::fprintf(stdout, "        thread %ld\n", id);

        if (blocks_asking(id)) {
            std::fprintf(stdout, "          blocks the signal\n");

            continue;
        }

        asked.store(id, std::memory_order_release);

        if (::syscall(SYS_tgkill, static_cast<long>(::getpid()), id, asking()) != 0) {
            asked.store(0, std::memory_order_release);
            std::fprintf(stdout, "          gone\n");

            continue;
        }

        const auto awaited = [](std::chrono::steady_clock::duration much) {
            const auto latest = std::chrono::steady_clock::now() + much;

            while (asked.load(std::memory_order_acquire) != 0 && std::chrono::steady_clock::now() < latest)
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
        };

        awaited(std::chrono::seconds{2});

        long open = id;

        // A request nobody claimed is taken back, so that a late answer writes nothing; a claimed
        // one is being written and gets two more seconds. A thread stuck inside its answer holds
        // the room for the stacks, so the telling ends there.
        if (asked.compare_exchange_strong(open, 0)) {
            std::fprintf(stdout, "          no answer\n");
            std::fflush(stdout);

            continue;
        }

        awaited(std::chrono::seconds{2});

        if (asked.load(std::memory_order_acquire) != 0) {
            std::fprintf(stdout, "          no answer\n");
            std::fflush(stdout);

            break;
        }

        Told one{id, {}};

        // The first two frames are the answer itself and the system's return from the signal.
        for (int at = 2; at < caught_much.load(std::memory_order_relaxed); ++at)
            one.frames.push_back(reinterpret_cast<std::uintptr_t>(caught[at]));

        for (const std::uintptr_t at : one.frames) std::fprintf(stdout, "          %s\n", placed(modules, at).c_str());

        std::fflush(stdout);

        made.push_back(std::move(one));
    }

    return made;
}

// One address as file, function and offset where the symbol table knows the function. The symbol
// must hold the address within its size: the nearest symbol before an address of a function that
// has none (an internal one) would name another function.
void name_told(std::uintptr_t at) {
    Dl_info about{};
    ElfW(Sym)* symbol = nullptr;

    if (::dladdr1(reinterpret_cast<void*>(at), &about, reinterpret_cast<void**>(&symbol), RTLD_DL_SYMENT) == 0) {
        std::fprintf(stdout, "          ? ?\n");

        return;
    }

    const char* file = about.dli_fname != nullptr ? about.dli_fname : "?";
    const char* slash = std::strrchr(file, '/');
    const char* module = slash != nullptr ? slash + 1 : file;
    const auto start = reinterpret_cast<std::uintptr_t>(about.dli_saddr);

    if (about.dli_sname == nullptr || symbol == nullptr || at < start || at >= start + symbol->st_size) {
        std::fprintf(stdout, "          %s ?\n", module);

        return;
    }

    int status = 0;
    char* plain = abi::__cxa_demangle(about.dli_sname, nullptr, nullptr, &status);

    std::fprintf(stdout, "          %s!%s+0x%llx\n", module, status == 0 && plain != nullptr ? plain : about.dli_sname,
                 static_cast<unsigned long long>(at - start));

    std::free(plain);
}

// The frames written before, now with the names of functions where the symbol table knows them
// (the libraries' exports, and the program's own, which the trial binaries export). It may wait for
// the loader's lock, so it comes second and runs on a thread of its own (`told_apart`).
void threads_named(const std::vector<Told>& all) {
    std::fprintf(stdout, "        the same, named:\n");

    for (const Told& one : all) {
        std::fprintf(stdout, "        thread %ld\n", one.id);

        for (const std::uintptr_t at : one.frames) name_told(at);

        std::fflush(stdout);
    }
}

#endif

#if defined(__linux__)

// Readies the answering of stack requests before any trial: the handler of the signal, and one
// call of `backtrace`, which loads the unwinder on its first call and must not do that inside a
// handler.
void readied() {
    struct sigaction how {};

    how.sa_handler = &answered;
    how.sa_flags = SA_RESTART;
    sigemptyset(&how.sa_mask);

    (void)::sigaction(asking(), &how, nullptr);

    void* once[1];

    (void)::backtrace(once, 1);
}

// The thread that writes the stacks, bare and then named, so that the watch can stop waiting for
// it.
void told_apart(std::atomic<bool>& finished) {
    const std::vector<Told> all = threads_told();

    threads_named(all);

    finished.store(true);
}

#else

// Nothing to ready where the watch writes no stacks on its own thread.
void readied() {}

#endif

// Ends the program at once with code 3. On Windows `_Exit` still goes through ExitProcess, which
// waits for the loader lock and for the libraries to detach, and so can hang behind the very
// thing that hung; TerminateProcess does not wait for anything.
[[noreturn]] void ended() {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 3);
#endif
    std::_Exit(3);
}

}

// Starts the watching thread; a trial may run for `TAL_TRIAL_LIMIT` seconds.
Watch::Watch() : limit_(longest()) {
    readied();

    thread_ = std::thread{[this] { watched(); }};
}

// Stops the watching thread.
Watch::~Watch() {
    {
        const std::lock_guard<std::mutex> lock{guard_};

        done_ = true;
    }

    wake_.notify_all();
    thread_.join();
}

// The trial `named` starts now; the time between two trials counts to the next one, and it is
// only the printing of a verdict.
void Watch::begun(std::string_view named) {
    const std::lock_guard<std::mutex> lock{guard_};

    named_ = std::string{named};
    since_ = std::chrono::steady_clock::now();
}

// Looks once a second whether the running trial has run longer than the limit. Then it names the
// trial and writes the stacks from a thread of its own, given twenty seconds, since naming the
// functions may wait for what hung.
void Watch::watched() {
    std::unique_lock<std::mutex> lock{guard_};

    while (!done_) {
        wake_.wait_for(lock, std::chrono::seconds{1});

        if (named_.empty() || std::chrono::steady_clock::now() - since_ <= limit_) continue;

        std::fflush(stdout);
        std::fprintf(stdout, "  HUNG  %s\n        still running after %d s\n", named_.c_str(), static_cast<int>(limit_.count()));
        std::fflush(stdout);

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__))
        if (const HANDLE telling = CreateThread(nullptr, 0, told_apart, nullptr, 0, nullptr); telling != nullptr) {
            (void)WaitForSingleObject(telling, 20000);
            CloseHandle(telling);
        }
#elif defined(__linux__)
        std::atomic<bool> finished{false};
        std::thread telling{[&finished] { told_apart(finished); }};
        const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{20};

        while (!finished.load() && std::chrono::steady_clock::now() < latest)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});

        telling.detach();
#endif

        std::fflush(stdout);
        ended();
    }
}

}
