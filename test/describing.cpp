#include "proving.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "talcite/meta.hpp"

// **Drzwi metadanych w rdzeniu.** Rozbioru formatów tu nie ma — mieszka za szwem, w talcite-adapters,
// i tam ma swoje próby. Tu pilnujemy samego szwu: że plik otwiera się w puli i to leniwie, że dostawca
// widzi prawdziwy rozmiar i prawdziwe bajty, że odczyt poza końcem jest krótszy, a nie błędny, i że
// pusty szew, odmowa dostawcy i brak pliku wychodzą trzema różnymi drogami.

using namespace talcite;

namespace {

namespace stdfs = std::filesystem;

[[nodiscard]] stdfs::path written(std::string_view name, std::string_view body) {
    const stdfs::path made = stdfs::temp_directory_path() / "talcite-describing";

    std::error_code why;

    stdfs::create_directories(made, why);

    const stdfs::path file = made / std::string{name};

    std::ofstream{file, std::ios::binary}.write(body.data(), static_cast<std::streamsize>(body.size()));

    return file;
}

// Udawany dostawca: typ to pierwsze cztery bajty, pole mówi rozmiar, drugie — dwa ostatnie bajty,
// trzecie — ile oddał odczyt zachodzący za koniec, czwarte — ile oddał odczyt w całości za końcem.
// Plik zaczynający się od „NOPE" odmawia. Obrazem jest bajt `which` powtórzony trzy razy.
class Faked final : public MetaProvider {
public:
    std::atomic<int> asked{0};

    [[nodiscard]] std::string_view name() const override { return "fake meta"; }

    [[nodiscard]] std::string describe(meta::Reader& from, meta::Description& into) override {
        ++asked;

        const std::string head = text(from.read(0, 4));

        if (head == "NOPE") return "not a file the fake would read";

        const std::uintmax_t size = from.size();

        into.type = head;
        into.fields.push_back({meta::Scheme::vorbis, "SIZE", static_cast<std::int64_t>(size)});
        into.fields.push_back({meta::Scheme::vorbis, "TAIL", text(from.read(size - 2, 2))});
        into.fields.push_back({meta::Scheme::vorbis, "OVER", static_cast<std::int64_t>(from.read(size - 3, 10).size())});
        into.fields.push_back({meta::Scheme::vorbis, "PAST", static_cast<std::int64_t>(from.read(size + 5, 10).size())});

        return {};
    }

    [[nodiscard]] std::string picture(meta::Reader& from, std::size_t which, Bytes& into) override {
        if (which > 9) return "no such picture";

        into.assign(3, std::byte{static_cast<std::uint8_t>(which)});
        into.push_back(from.read(0, 1).at(0));

        return {};
    }
};

}

TRIAL("Metadane: pusty szew odrzuca obietnicę Unbound z nazwą rodzaju") {
    const Seam<MetaProvider> none;

    ought(!none.bound());

    const stdfs::path file = written("unbound.bin", "WAVEdata");

    bool amiss = false;

    try {
        (void)none.described(file).await();
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for Meta.";
    }

    ought(amiss);

    amiss = false;

    try {
        (void)none.pictured(file, 0).await();
    } catch (const Unbound&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Metadane: dostawca widzi rozmiar i bajty pliku, a odczyt za końcem jest krótszy") {
    auto faked = std::make_shared<Faked>();
    const Seam<MetaProvider> meta{faked};

    const stdfs::path file = written("plain.bin", "fLaC....xyz");

    Promise<meta::Description> asked = meta.described(file);

    same(faked->asked.load(), 0);

    const meta::Description got = asked.await();

    same(faked->asked.load(), 1);
    same(got.type, std::string{"fLaC"});
    same(got.fields.size(), std::size_t{4});
    ought(got.fields[0].value == meta::Value{std::int64_t{11}});
    ought(got.fields[1].value == meta::Value{std::string{"yz"}});
    ought(got.fields[2].value == meta::Value{std::int64_t{3}});
    ought(got.fields[3].value == meta::Value{std::int64_t{0}});
    ought(!got.damaged);
}

TRIAL("Metadane: odmowa dostawcy wychodzi jako Refused z nazwą rodzaju") {
    const Seam<MetaProvider> meta{std::make_shared<Faked>()};

    same(std::string{meta.provider().name()}, std::string{"fake meta"});

    bool amiss = false;

    try {
        (void)meta.described(written("refused.bin", "NOPE and more")).await();
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "Meta provider refused: not a file the fake would read";
    }

    ought(amiss);

    amiss = false;

    try {
        (void)meta.pictured(written("pictured.bin", "abc"), 10).await();
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "Meta provider refused: no such picture";
    }

    ought(amiss);
}

TRIAL("Metadane: brak pliku odrzuca błędem systemu plików, a dostawca nie jest pytany") {
    auto faked = std::make_shared<Faked>();
    const Seam<MetaProvider> meta{faked};

    bool amiss = false;

    try {
        (void)meta.described(stdfs::temp_directory_path() / "talcite-describing" / "nie ma.bin").await();
    } catch (const fs::Trouble&) {
        amiss = true;
    }

    ought(amiss);
    same(faked->asked.load(), 0);
}

TRIAL("Metadane: obraz idzie od dostawcy z numerem, o który pytano") {
    const Seam<MetaProvider> meta{std::make_shared<Faked>()};

    const Bytes got = meta.pictured(written("picture.bin", "Zebra"), 7).await();

    same(got.size(), std::size_t{4});
    same(std::to_integer<int>(got[0]), 7);
    same(std::to_integer<int>(got[2]), 7);
    same(std::to_integer<char>(got[3]), 'Z');
}
