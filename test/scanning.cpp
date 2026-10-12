#include "proving.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "talcite/scan.hpp"

#ifndef _WIN32
#include <signal.h>
#endif

// Scanning with the system's antivirus program. There is no real antivirus here, so on POSIX we
// put two programs with ClamAV names on PATH: a daemon that does not work (exit code 2) and a
// scanner whose exit code says whether the file holds the "infection". On Windows we only check
// that the scan answers and breaks nothing; the result depends on the machine's antivirus.

using namespace talcite;

namespace {

namespace stdfs = std::filesystem;

#ifndef _WIN32

struct Varied {
    // Sets the environment variable `name` to `value` for the lifetime of the object and remembers
    // the previous value.
    Varied(const char* name, std::string value) : name_(name) {
        if (const char* was = std::getenv(name)) was_ = std::string{was};

        ::setenv(name, value.c_str(), 1);
    }

    // Restores the variable's previous value, or removes it when it was not set before.
    ~Varied() {
        if (was_) ::setenv(name_, was_->c_str(), 1);
        else ::unsetenv(name_);
    }

    Varied(const Varied&) = delete;
    Varied& operator=(const Varied&) = delete;

private:
    const char* name_;
    std::optional<std::string> was_;
};

// Writes a shell script with the given body at `where` and makes it executable by the owner.
void program(const stdfs::path& where, const std::string& body) {
    std::ofstream{where, std::ios::binary} << "#!/bin/sh\n" << body;

    stdfs::permissions(where, stdfs::perms::owner_all, stdfs::perm_options::replace);
}

#endif

// An empty temporary folder with the given name, wiped first if it is left over from an earlier run.
stdfs::path folder(const std::string& name) {
    const stdfs::path made = stdfs::temp_directory_path() / name;

    std::error_code why;

    stdfs::remove_all(made, why);
    stdfs::create_directories(made, why);

    return made;
}

}

TRIAL("Scan: clean, threat and missing scanner give different answers") {
    const stdfs::path root = folder("talcite-skan");

    std::ofstream{root / "film.mkv", std::ios::binary} << "zwykła treść";
    std::ofstream{root / "groźny.exe", std::ios::binary} << "ZARAZA";

#ifdef _WIN32
    const scan::Scanned one = scan::scanned(root / "film.mkv").get();

    ought(one.verdict != scan::Verdict::infected);
#else
    const stdfs::path bin = root / "bin";

    stdfs::create_directories(bin);

    // A ClamAV daemon that does not work, and a scanner that looks for the text in its last argument.
    program(bin / "clamdscan", "echo 'ERROR: Could not connect'\nexit 2\n");
    program(bin / "clamscan", "if grep -q ZARAZA \"$3\"; then echo \"$3: Zaraza FOUND\"; exit 1; fi\nexit 0\n");

    {
        const Varied path{"PATH", fs::to_string(bin) + ":/usr/bin:/bin"};

        const scan::Scanned clean = scan::scanned(root / "film.mkv").get();
        const scan::Scanned bad = scan::scanned(root / "groźny.exe").get();

        ought(clean.verdict == scan::Verdict::clean);
        same(clean.by, std::string{"clamscan"});
        ought(bad.verdict == scan::Verdict::infected);
        ought(bad.said.find("FOUND") != std::string::npos);
        ought(!bad.said.empty() && bad.said.back() != '\n');

        // A working daemon is enough; the scanner is no longer asked.
        program(bin / "clamdscan", "if grep -q ZARAZA \"$3\"; then exit 1; fi\nexit 0\n");
        program(bin / "clamscan", "exit 2\n");

        same(scan::scanned(root / "groźny.exe").get().by, std::string{"clamdscan"});
        ought(scan::scanned(root / "film.mkv").get().verdict == scan::Verdict::clean);

        // A scanner without a database (exit code 2): the result is unknown, not "clean".
        program(bin / "clamdscan", "exit 2\n");

        ought(scan::scanned(root / "film.mkv").get().verdict == scan::Verdict::unknown);

        // A scanner killed by a signal said nothing, even if the signal number looks like a threat code.
        // Under nohup SIGHUP is ignored, and every program started from here inherits that, so the
        // scanner would not die and the check would be about nothing.
        struct sigaction hangup{};

        if (::sigaction(SIGHUP, nullptr, &hangup) == 0 && hangup.sa_handler != SIG_IGN) {
            program(bin / "clamscan", "kill -HUP $$\n");

            ought(scan::scanned(root / "film.mkv").get().verdict == scan::Verdict::unknown);
        }
    }

    {
        const Varied path{"PATH", fs::to_string(root / "nic")};

        const scan::Scanned none = scan::scanned(root / "film.mkv").get();

        ought(none.verdict == scan::Verdict::unknown);
        ought(none.by.empty());
    }
#endif
}
