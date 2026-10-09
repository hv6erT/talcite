#include "proving.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "talcite/scan.hpp"

// **Skan programem antywirusowym systemu.** Prawdziwego programu antywirusowego tu nie ma, więc na
// POSIX-ie podkładamy w PATH dwa programy o nazwach ClamAV: demon, który nie działa (kod 2), i skaner,
// który kodem wyjścia mówi, czy w pliku jest „zaraza". Na Windows — tylko to, że skan odpowiada
// i niczego nie wywraca; wynik zależy od programu antywirusowego maszyny.

using namespace talcite;

namespace {

namespace stdfs = std::filesystem;

#ifndef _WIN32

struct Varied {
    Varied(const char* name, std::string value) : name_(name) {
        if (const char* was = std::getenv(name)) was_ = std::string{was};

        ::setenv(name, value.c_str(), 1);
    }

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

void program(const stdfs::path& where, const std::string& body) {
    std::ofstream{where, std::ios::binary} << "#!/bin/sh\n" << body;

    stdfs::permissions(where, stdfs::perms::owner_all, stdfs::perm_options::replace);
}

#endif

stdfs::path folder(const std::string& name) {
    const stdfs::path made = stdfs::temp_directory_path() / name;

    std::error_code why;

    stdfs::remove_all(made, why);
    stdfs::create_directories(made, why);

    return made;
}

}

TRIAL("Skan: czysty, zagrożenie i brak skanera mówią różnie") {
    const stdfs::path root = folder("talcite-skan");

    std::ofstream{root / "film.mkv", std::ios::binary} << "zwykła treść";
    std::ofstream{root / "groźny.exe", std::ios::binary} << "ZARAZA";

#ifdef _WIN32
    const scan::Scanned one = scan::scanned(root / "film.mkv").await();

    ought(one.verdict != scan::Verdict::infected);
#else
    const stdfs::path bin = root / "bin";

    stdfs::create_directories(bin);

    // Demon ClamAV, który nie działa, i skaner, który szuka napisu w ostatnim argumencie.
    program(bin / "clamdscan", "echo 'ERROR: Could not connect'\nexit 2\n");
    program(bin / "clamscan", "if grep -q ZARAZA \"$3\"; then echo \"$3: Zaraza FOUND\"; exit 1; fi\nexit 0\n");

    {
        const Varied path{"PATH", fs::to_string(bin) + ":/usr/bin:/bin"};

        const scan::Scanned clean = scan::scanned(root / "film.mkv").await();
        const scan::Scanned bad = scan::scanned(root / "groźny.exe").await();

        ought(clean.verdict == scan::Verdict::clean);
        same(clean.by, std::string{"clamscan"});
        ought(bad.verdict == scan::Verdict::infected);
        ought(bad.said.find("FOUND") != std::string::npos);
        ought(!bad.said.empty() && bad.said.back() != '\n');

        // Demon, który działa, wystarcza — skaner już nie jest pytany.
        program(bin / "clamdscan", "if grep -q ZARAZA \"$3\"; then exit 1; fi\nexit 0\n");
        program(bin / "clamscan", "exit 2\n");

        same(scan::scanned(root / "groźny.exe").await().by, std::string{"clamdscan"});
        ought(scan::scanned(root / "film.mkv").await().verdict == scan::Verdict::clean);

        // Skaner bez bazy (kod 2): nie wiadomo — nie „czysto".
        program(bin / "clamdscan", "exit 2\n");

        ought(scan::scanned(root / "film.mkv").await().verdict == scan::Verdict::unknown);

        // Skaner zabity sygnałem nie powiedział nic — choćby numer sygnału wyglądał jak kod zagrożenia.
        program(bin / "clamscan", "kill -HUP $$\n");

        ought(scan::scanned(root / "film.mkv").await().verdict == scan::Verdict::unknown);
    }

    {
        const Varied path{"PATH", fs::to_string(root / "nic")};

        const scan::Scanned none = scan::scanned(root / "film.mkv").await();

        ought(none.verdict == scan::Verdict::unknown);
        ought(none.by.empty());
    }
#endif
}
