#include "tal/testing.hpp"

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

// The metadata door in the core. There is no format parsing here; it lives behind the seam, in
// talcite-adapters, and has its trials there. Here we guard the seam itself: that the file is
// opened in the pool and lazily, that the provider sees the real size and the real bytes, that a
// read past the end is shorter rather than an error, and that an empty seam, a provider's refusal
// and a missing file come out three different ways.

using namespace talcite;

namespace {

namespace stdfs = std::filesystem;

// Writes `body` to the file `name` in a directory of the trials and returns its path.
[[nodiscard]] stdfs::path written(std::string_view name, std::string_view body) {
    const stdfs::path made = stdfs::temp_directory_path() / "talcite-describing";

    std::error_code why;

    stdfs::create_directories(made, why);

    const stdfs::path file = made / std::string{name};

    std::ofstream{file, std::ios::binary}.write(body.data(), static_cast<std::streamsize>(body.size()));

    return file;
}

// A fake provider: the type is the first four bytes, the first field tells the size, the second
// the last two bytes, the third how much a read reaching past the end returned, the fourth how
// much a read entirely past the end returned. A file starting with "NOPE" is refused. A picture
// is the byte `which` repeated three times, followed by the file's first byte.
class Faked final : public MetaProvider {
public:
    std::atomic<int> asked{0};

    // The name of the fake provider.
    [[nodiscard]] std::string_view name() const override { return "fake meta"; }

    // Fills in the description as above and counts the call in `asked`.
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

    // Gives picture `which`, or refuses for a number above nine.
    [[nodiscard]] std::string picture(meta::Reader& from, std::size_t which, Bytes& into) override {
        if (which > 9) return "no such picture";

        into.assign(3, std::byte{static_cast<std::uint8_t>(which)});
        into.push_back(from.read(0, 1).at(0));

        return {};
    }
};

}

TRIAL("Meta: an empty seam rejects the promise with Unbound naming the kind") {
    const Seam<MetaProvider> none;

    OUGHT(!none.bound());

    const stdfs::path file = written("unbound.bin", "WAVEdata");

    bool amiss = false;

    try {
        (void)none.described(file).get();
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for Meta.";
    }

    OUGHT(amiss);

    amiss = false;

    try {
        (void)none.pictured(file, 0).get();
    } catch (const Unbound&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Meta: the provider sees the file's size and bytes, and a read past the end is shorter") {
    auto faked = std::make_shared<Faked>();
    const Seam<MetaProvider> meta{faked};

    const stdfs::path file = written("plain.bin", "fLaC....xyz");

    const meta::Description got = meta.described(file).get();

    SAME(faked->asked.load(), 1);
    SAME(got.type, std::string{"fLaC"});
    SAME(got.fields.size(), std::size_t{4});
    OUGHT(got.fields[0].value == meta::Value{std::int64_t{11}});
    OUGHT(got.fields[1].value == meta::Value{std::string{"yz"}});
    OUGHT(got.fields[2].value == meta::Value{std::int64_t{3}});
    OUGHT(got.fields[3].value == meta::Value{std::int64_t{0}});
    OUGHT(!got.damaged);
}

TRIAL("Meta: the provider's refusal comes out as Refused naming the kind") {
    const Seam<MetaProvider> meta{std::make_shared<Faked>()};

    SAME(std::string{meta.provider().name()}, std::string{"fake meta"});

    bool amiss = false;

    try {
        (void)meta.described(written("refused.bin", "NOPE and more")).get();
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "Meta provider refused: not a file the fake would read";
    }

    OUGHT(amiss);

    amiss = false;

    try {
        (void)meta.pictured(written("pictured.bin", "abc"), 10).get();
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "Meta provider refused: no such picture";
    }

    OUGHT(amiss);
}

TRIAL("Meta: a missing file rejects with a file system error, and the provider is not asked") {
    auto faked = std::make_shared<Faked>();
    const Seam<MetaProvider> meta{faked};

    bool amiss = false;

    try {
        (void)meta.described(stdfs::temp_directory_path() / "talcite-describing" / "nie ma.bin").get();
    } catch (const fs::Trouble&) {
        amiss = true;
    }

    OUGHT(amiss);
    SAME(faked->asked.load(), 0);
}

TRIAL("Meta: the picture comes from the provider with the number asked for") {
    const Seam<MetaProvider> meta{std::make_shared<Faked>()};

    const Bytes got = meta.pictured(written("picture.bin", "Zebra"), 7).get();

    SAME(got.size(), std::size_t{4});
    SAME(std::to_integer<int>(got[0]), 7);
    SAME(std::to_integer<int>(got[2]), 7);
    SAME(std::to_integer<char>(got[3]), 'Z');
}
