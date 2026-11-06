#include "proving.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "talcite/archive.hpp"
#include "talcite/bytes.hpp"
#include "talcite/fs.hpp"
#include "talcite/seam.hpp"
#include "talcite/unpack.hpp"

using namespace talcite;

// Archives built byte by byte here, ZIP with stored members and tar, so that every field the
// reader looks at is set on purpose: names safe and unsafe, ZIP64, a program before the archive,
// code page 437, the extended timestamp, pax and GNU long names, links. Deflate and gzip themselves
// are the unpacker's, and talcite-adapters tries them with miniz; here a stand-in that passes the
// bytes on shows that a .tar.gz goes through the unpacker at all.

namespace {

namespace stood = std::filesystem;

// A fresh empty directory, removed with what it holds.
class Nook {
public:
    // Creates the directory under a name unique to this process and object.
    Nook() {
        static std::atomic<int> counted{0};

        where_ = stood::temp_directory_path() /
                 stood::path{"talcite-archive-" + std::to_string(++counted) + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this))};

        std::error_code ignored;

        stood::remove_all(where_, ignored);
        stood::create_directories(where_, ignored);
    }

    // Removes the directory, ignoring errors.
    ~Nook() {
        std::error_code ignored;

        stood::remove_all(where_, ignored);
    }

    Nook(const Nook&) = delete;
    Nook& operator=(const Nook&) = delete;

    // The directory.
    [[nodiscard]] const stood::path& where() const { return where_; }

private:
    stood::path where_;
};

// The CRC-32 of the text, bit by bit, apart from the reader's table.
std::uint32_t crc_of(std::string_view text) {
    std::uint32_t made = 0xFFFFFFFFU;

    for (const char one : text) {
        made ^= static_cast<unsigned char>(one);

        for (int bit = 0; bit < 8; ++bit) made = (made & 1U) != 0 ? (made >> 1) ^ 0xEDB88320U : made >> 1;
    }

    return ~made;
}

// Appends a little-endian number of `much` bytes.
void le(Bytes& into, std::uint64_t value, std::size_t much) {
    for (std::size_t at = 0; at < much; ++at) into.push_back(static_cast<std::byte>((value >> (8 * at)) & 0xFF));
}

// Appends the text.
void text(Bytes& into, std::string_view what) {
    for (const char one : what) into.push_back(static_cast<std::byte>(one));
}

// One entry of a ZIP built here, stored: its name as bytes, its content, and what may differ from
// an ordinary file of a Unix packer. With `unpacked` the content is packed (method 8, through the
// stand-in that doubles every byte) and `unpacked` is what comes out. `declared` is a size the
// central directory claims instead of the true one, and `at` an offset it gives instead of the
// entry's own, as a zip bomb's do.
struct Entry {
    std::string name;
    std::string data;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::uint16_t made_by = 0x031E;
    std::uint32_t outer = 0100644U << 16;
    std::optional<std::int32_t> stamp;
    std::optional<std::uint32_t> crc;
    bool wide = false;
    std::optional<std::string> unpacked;
    std::optional<std::uint64_t> declared;
    std::optional<std::uint64_t> at;
};

// A ZIP of the entries, after `prefix` (a program before it, say). Every entry is stored; one
// marked `wide` writes its sizes and offset through ZIP64, and then so does the end.
Bytes zipped(const std::vector<Entry>& entries, std::string_view prefix = {}) {
    Bytes made;
    Bytes central;

    text(made, prefix);

    bool zip64 = false;

    for (const Entry& one : entries) {
        const std::uint64_t offset = one.at.value_or(made.size() - prefix.size());
        const std::string& whole = one.unpacked ? *one.unpacked : one.data;
        const std::uint64_t told = one.declared.value_or(whole.size());
        const std::uint32_t crc = one.crc.value_or(crc_of(whole));

        Bytes extra;

        if (one.stamp) {
            le(extra, 0x5455, 2);
            le(extra, 5, 2);
            extra.push_back(std::byte{1});
            le(extra, static_cast<std::uint32_t>(*one.stamp), 4);
        }

        Bytes wide;

        if (one.wide) {
            zip64 = true;

            le(wide, 0x0001, 2);
            le(wide, 24, 2);
            le(wide, told, 8);
            le(wide, one.data.size(), 8);
            le(wide, offset, 8);
        }

        le(made, 0x04034b50, 4);
        le(made, 20, 2);
        le(made, one.flags, 2);
        le(made, one.method, 2);
        le(made, 0, 4);
        le(made, crc, 4);
        le(made, one.data.size(), 4);
        le(made, whole.size(), 4);
        le(made, one.name.size(), 2);
        le(made, 0, 2);
        text(made, one.name);
        text(made, one.data);

        le(central, 0x02014b50, 4);
        le(central, one.made_by, 2);
        le(central, 20, 2);
        le(central, one.flags, 2);
        le(central, one.method, 2);
        le(central, 0x6000, 2);
        le(central, 0x5821, 2);
        le(central, crc, 4);
        le(central, one.wide ? 0xFFFFFFFFU : one.data.size(), 4);
        le(central, one.wide ? 0xFFFFFFFFU : told, 4);
        le(central, one.name.size(), 2);
        le(central, extra.size() + wide.size(), 2);
        le(central, 0, 2);
        le(central, 0, 2);
        le(central, 0, 2);
        le(central, one.outer, 4);
        le(central, one.wide ? 0xFFFFFFFFU : offset, 4);
        text(central, one.name);
        central.insert(central.end(), extra.begin(), extra.end());
        central.insert(central.end(), wide.begin(), wide.end());
    }

    const std::uint64_t directory = made.size() - prefix.size();

    made.insert(made.end(), central.begin(), central.end());

    if (zip64) {
        const std::uint64_t end64 = made.size() - prefix.size();

        le(made, 0x06064b50, 4);
        le(made, 44, 8);
        le(made, 45, 2);
        le(made, 45, 2);
        le(made, 0, 4);
        le(made, 0, 4);
        le(made, entries.size(), 8);
        le(made, entries.size(), 8);
        le(made, central.size(), 8);
        le(made, directory, 8);

        le(made, 0x07064b50, 4);
        le(made, 0, 4);
        le(made, end64, 8);
        le(made, 1, 4);
    }

    le(made, 0x06054b50, 4);
    le(made, 0, 2);
    le(made, 0, 2);
    le(made, zip64 ? 0xFFFF : entries.size(), 2);
    le(made, zip64 ? 0xFFFF : entries.size(), 2);
    le(made, zip64 ? 0xFFFFFFFFU : central.size(), 4);
    le(made, zip64 ? 0xFFFFFFFFU : directory, 4);
    le(made, 7, 2);
    text(made, "komentarz");

    made.resize(made.size() - 2);

    return made;
}

// One member of a tar built here: its kind as the type byte, name, content, time and mode, and a
// size the header claims instead of the content's (`told`).
struct Tarred {
    char type = '0';
    std::string name;
    std::string data;
    std::uint64_t mtime = 1'700'000'000;
    unsigned mode = 0644;
    std::string link;
    std::optional<std::uint64_t> told;
};

// The octal field of `much` bytes holding `value`, NUL at the end.
void octal(Bytes& header, std::size_t at, std::size_t much, std::uint64_t value) {
    std::string digits(much - 1, '0');

    for (std::size_t one = much - 1; one-- > 0;) {
        digits[one] = static_cast<char>('0' + (value & 7U));
        value >>= 3;
    }

    for (std::size_t one = 0; one < much - 1; ++one) header[at + one] = static_cast<std::byte>(digits[one]);

    header[at + much - 1] = std::byte{0};
}

// A ustar header for the member, with its checksum, and the data padded to 512 bytes.
void tarred(Bytes& into, const Tarred& one) {
    Bytes header(512, std::byte{0});

    std::copy_n(reinterpret_cast<const std::byte*>(one.name.data()), std::min<std::size_t>(one.name.size(), 100), header.begin());

    octal(header, 100, 8, one.mode);
    octal(header, 108, 8, 1000);
    octal(header, 116, 8, 1000);
    octal(header, 124, 12, one.told.value_or(one.data.size()));
    octal(header, 136, 12, one.mtime);

    header[156] = static_cast<std::byte>(one.type);

    std::copy_n(reinterpret_cast<const std::byte*>(one.link.data()), std::min<std::size_t>(one.link.size(), 100), header.begin() + 157);

    for (std::size_t at = 0; at < 6; ++at) header[257 + at] = static_cast<std::byte>("ustar"[at]);

    header[263] = std::byte{'0'};
    header[264] = std::byte{'0'};

    for (std::size_t at = 148; at < 156; ++at) header[at] = std::byte{' '};

    unsigned sum = 0;

    for (const std::byte each : header) sum += std::to_integer<unsigned>(each);

    octal(header, 148, 7, sum);
    header[155] = std::byte{' '};

    into.insert(into.end(), header.begin(), header.end());
    text(into, one.data);
    into.resize(into.size() + (512 - one.data.size() % 512) % 512, std::byte{0});
}

// A tar of the members, closed by two blocks of zeros.
Bytes tar_of(const std::vector<Tarred>& members) {
    Bytes made;

    for (const Tarred& one : members) tarred(made, one);

    made.resize(made.size() + 1024, std::byte{0});

    return made;
}

// Writes the bytes to the file.
void put(const stood::path& where, const Bytes& what) {
    std::ofstream out{where, std::ios::binary};

    out.write(reinterpret_cast<const char*>(what.data()), static_cast<std::streamsize>(what.size()));
}

// The text of the file, empty when it cannot be read.
std::string read(const stood::path& where) {
    std::ifstream in{where, std::ios::binary};

    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The names of the members in UTF-8, with slashes on every system.
std::vector<std::string> names(const archive::Archive& one) {
    std::vector<std::string> made;

    for (const archive::Member& each : one.members()) {
        std::string name = fs::to_string(each.name);

        std::replace(name.begin(), name.end(), '\\', '/');

        made.push_back(std::move(name));
    }

    return made;
}

// A pax record, "length key=value\n", its length counting itself.
std::string pax(std::string_view key, std::string_view value) {
    const std::size_t rest = key.size() + value.size() + 3;

    std::size_t length = rest + 1;

    while (std::to_string(length).size() + rest != length) ++length;

    return std::to_string(length) + " " + std::string{key} + "=" + std::string{value} + "\n";
}

// The member with the name; it must be there.
const archive::Member& member(const archive::Archive& one, std::string_view name) {
    static const archive::Member none;

    const std::optional<std::size_t> at = one.found(stood::path{name});

    ought(at.has_value());

    return at ? one.members()[*at] : none;
}

// The error an operation ends with, or nothing.
template <typename T>
std::string trouble(Promise<T> one) {
    try {
        static_cast<void>(std::move(one).get());
    } catch (const std::exception& why) {
        return why.what();
    }

    return "";
}

// A "gzip" that passes the bytes on as they are and counts them, for a .tar.gz that is a tar.
class Passing : public Unpacking {
public:
    explicit Passing(std::shared_ptr<std::size_t> fed) : fed_(std::move(fed)) {}

    // The bytes, as they came.
    [[nodiscard]] Bytes fed(std::span<const std::byte> given) override {
        *fed_ += given.size();

        return Bytes(given.begin(), given.end());
    }

    // Always whole.
    [[nodiscard]] bool ended() const override { return true; }

private:
    std::shared_ptr<std::size_t> fed_;
};

// A "deflate" that doubles every byte, so that a member's packed size differs from its size.
class Doubling : public Unpacking {
public:
    // The bytes, each twice.
    [[nodiscard]] Bytes fed(std::span<const std::byte> given) override {
        Bytes made;

        for (const std::byte one : given) made.insert(made.end(), 2, one);

        return made;
    }

    // Always whole.
    [[nodiscard]] bool ended() const override { return true; }
};

// A "gzip" that swells: the first piece passes as it is (the tar header), every later one gives a
// block of zeros a millisecond later, however small it was, and the pieces are counted. A tar
// member claiming gigabytes of such zeros keeps the reading busy for as long as it is let run.
class Swelling : public Unpacking {
public:
    explicit Swelling(std::shared_ptr<std::atomic<std::size_t>> fed) : fed_(std::move(fed)) {}

    // The first piece as it came, then 64 KiB of zeros per piece.
    [[nodiscard]] Bytes fed(std::span<const std::byte> given) override {
        if (fed_->fetch_add(1) == 0) return Bytes(given.begin(), given.end());

        std::this_thread::sleep_for(std::chrono::milliseconds{1});

        return Bytes(64 * 1024, std::byte{0});
    }

    // Never whole.
    [[nodiscard]] bool ended() const override { return false; }

private:
    std::shared_ptr<std::atomic<std::size_t>> fed_;
};

// The unpacker of `Swelling`, for gzip only.
class Sweller : public UnpackProvider {
public:
    // Counts into `fed`.
    explicit Sweller(std::shared_ptr<std::atomic<std::size_t>> fed) : fed_(std::move(fed)) {}

    // The provider's name.
    [[nodiscard]] std::string_view name() const override { return "swelling"; }

    // A swelling unpacker.
    [[nodiscard]] std::unique_ptr<Unpacking> started(Packing which) override {
        ought(which == Packing::gzip);

        return std::make_unique<Swelling>(fed_);
    }

private:
    std::shared_ptr<std::atomic<std::size_t>> fed_;
};

// The unpacker of `Doubling`, for deflate only.
class Doubler : public UnpackProvider {
public:
    // The provider's name.
    [[nodiscard]] std::string_view name() const override { return "doubling"; }

    // A doubling unpacker.
    [[nodiscard]] std::unique_ptr<Unpacking> started(Packing which) override {
        ought(which == Packing::deflate);

        return std::make_unique<Doubling>();
    }
};

// The unpacker of `Passing`.
class Passer : public UnpackProvider {
public:
    // Counts into `fed`.
    explicit Passer(std::shared_ptr<std::size_t> fed) : fed_(std::move(fed)) {}

    // The provider's name.
    [[nodiscard]] std::string_view name() const override { return "passing"; }

    // A passing unpacker, for gzip only.
    [[nodiscard]] std::unique_ptr<Unpacking> started(Packing which) override {
        ought(which == Packing::gzip);

        return std::make_unique<Passing>(fed_);
    }

private:
    std::shared_ptr<std::size_t> fed_;
};

}

TRIAL("Archive: the format comes from the end of the name") {
    ought(archive::format_of("napisy.zip") == archive::Format::zip);
    ought(archive::format_of("NAPISY.ZIP") == archive::Format::zip);
    ought(archive::format_of("kopia.tar") == archive::Format::tar);
    ought(archive::format_of("kopia.tar.gz") == archive::Format::tar_gzip);
    ought(archive::format_of("kopia.TGZ") == archive::Format::tar_gzip);
    ought(!archive::format_of("napisy.srt.gz").has_value());
    ought(!archive::format_of(".zip").has_value());
    ought(!archive::format_of("film.mkv").has_value());
}

// The members of a ZIP with their sizes, kinds, times and rights; folders no entry names come
// from the names; a member is found by its name, and the direct content of a folder is asked for.
TRIAL("Archive: a ZIP lists its members, folders implied by the names too") {
    const Nook nook;
    const stood::path where = nook.where() / "napisy.zip";

    Entry folder{"Polskie/", ""};

    folder.outer = 040755U << 16;

    Entry first{"Polskie/Heat.srt", "napisy do Heat"};

    first.stamp = 1'700'000'000;

    put(where, zipped({folder, first, Entry{"Angielskie/Lata/Ronin.srt", "subtitles"}, Entry{"czytaj.txt", "x"}}));

    const archive::Archive got = archive::opened(where, {}).get();

    same(names(got), (std::vector<std::string>{"Polskie", "Polskie/Heat.srt", "Angielskie/Lata/Ronin.srt", "czytaj.txt", "Angielskie/Lata",
                                               "Angielskie"}));
    ought(got.format() == archive::Format::zip);
    ought(member(got, "Polskie").kind == fs::Kind::folder);
    same(member(got, "Polskie/Heat.srt").size, std::uintmax_t{14});
    same(member(got, "Polskie/Heat.srt").modified.time_since_epoch().count(), std::int64_t{1'700'000'000});
    ought(member(got, "Polskie/Heat.srt").rights == stood::perms{0644});
    ought(member(got, "Angielskie").kind == fs::Kind::folder);
    ought(member(got, "Polskie/Heat.srt").refusal == archive::Refusal::none);

    std::vector<std::string> top;

    for (const std::size_t at : got.within({})) top.push_back(fs::to_string(got.members()[at].name));

    same(top, (std::vector<std::string>{"Polskie", "czytaj.txt", "Angielskie"}));
    same(got.within("Angielskie").size(), std::size_t{1});
    ought(!got.found("Brak").has_value());
}

// Names that would write outside the place given stay listed, refused; so do links, encrypted
// members and methods the reader does not know. Extracting the whole archive leaves them out and
// says which, and nothing appears outside the target.
TRIAL("Archive: unsafe names, links, encryption and unknown methods are refused, and extraction keeps to its place") {
    const Nook nook;
    const stood::path where = nook.where() / "zly.zip";

    Entry link{"skrot", "cel"};

    link.outer = 0120777U << 16;

    Entry hidden{"tajne.txt", "x"};

    hidden.flags = 1;

    Entry packed{"bzip.txt", "x"};

    packed.method = 12;

    put(where, zipped({Entry{"../poza.txt", "a"}, Entry{"/korzen.txt", "b"}, Entry{"C:/dysk.txt", "c"}, Entry{"a\\..\\..\\b.txt", "d"},
                       Entry{"strumien.txt:ukryty", "e"}, link, hidden, packed, Entry{"dobry.txt", "f"}}));

    const archive::Archive got = archive::opened(where, {}).get();

    for (const archive::Member& one : got.members())
        if (one.stored == "../poza.txt" || one.stored == "/korzen.txt" || one.stored == "C:/dysk.txt" || one.stored == "a\\..\\..\\b.txt" ||
            one.stored == "strumien.txt:ukryty")
            ought(one.refusal == archive::Refusal::unsafe);

    ought(member(got, "skrot").refusal == archive::Refusal::link);
    ought(member(got, "tajne.txt").refusal == archive::Refusal::encrypted);
    ought(member(got, "bzip.txt").refusal == archive::Refusal::method);

    const stood::path to = nook.where() / "cel" / "w";

    stood::create_directories(to.parent_path());

    const std::vector<archive::Member> left = got.extracted({archive::Pick{{}, to}}).get();

    same(left.size(), std::size_t{8});
    same(read(to / "dobry.txt"), std::string{"f"});

    std::size_t there = 0;

    for (auto at = stood::recursive_directory_iterator{nook.where() / "cel"}; at != stood::recursive_directory_iterator{}; ++at) ++there;

    same(there, std::size_t{2});
    ought(!stood::exists(nook.where() / "poza.txt"));
}

// The contents go where the picks say, with the archive's times; a file's own pick writes just it.
// A member whose bytes do not match its CRC-32 is refused, and nothing of it stays.
TRIAL("Archive: extraction writes files whole with their times, and a wrong CRC leaves nothing") {
    const Nook nook;
    const stood::path where = nook.where() / "a.zip";

    Entry dated{"film/opis.txt", "opis filmu"};

    dated.stamp = 1'600'000'000;

    Entry wrong{"zly.txt", "treść"};

    wrong.crc = 12345;

    put(where, zipped({dated, Entry{"film/plakat/maly.txt", "maly"}, wrong}));

    const archive::Archive got = archive::opened(where, {}).get();
    const stood::path to = nook.where() / "film";

    same(got.extracted({archive::Pick{"film", to}}).get().size(), std::size_t{0});
    same(read(to / "opis.txt"), std::string{"opis filmu"});
    same(read(to / "plakat" / "maly.txt"), std::string{"maly"});
    same(fs::modified(to / "opis.txt").get().time_since_epoch().count(), std::int64_t{1'600'000'000});

    const std::string why = trouble(got.extracted({archive::Pick{"zly.txt", nook.where() / "zly.txt"}}));

    ought(why.find("CRC-32") != std::string::npos);
    ought(!stood::exists(nook.where() / "zly.txt"));

    std::size_t parts = 0;

    for (const auto& one : stood::directory_iterator{nook.where()})
        if (one.path().extension() == ".part") ++parts;

    same(parts, std::size_t{0});
}

// A conflict with what stands there is settled by the rule given, file by file, and for a folder
// pick once for the folder: refused, skipped, replaced or written beside.
TRIAL("Archive: a conflict is refused, skipped, overwritten or renamed") {
    const Nook nook;
    const stood::path where = nook.where() / "a.zip";

    put(where, zipped({Entry{"a.txt", "nowe"}, Entry{"d/b.txt", "nowe b"}}));

    const archive::Archive got = archive::opened(where, {}).get();
    const stood::path target = nook.where() / "a.txt";

    put(target, talcite::bytes("stare"));

    ought(!trouble(got.extracted({archive::Pick{"a.txt", target}})).empty());
    same(read(target), std::string{"stare"});

    static_cast<void>(got.extracted({archive::Pick{"a.txt", target}}, fs::Conflict::skip).get());
    same(read(target), std::string{"stare"});

    static_cast<void>(got.extracted({archive::Pick{"a.txt", target}}, fs::Conflict::rename).get());
    same(read(nook.where() / "a (2).txt"), std::string{"nowe"});

    static_cast<void>(got.extracted({archive::Pick{"a.txt", target}}, fs::Conflict::overwrite).get());
    same(read(target), std::string{"nowe"});

    stood::create_directories(nook.where() / "d");
    put(nook.where() / "d" / "c.txt", talcite::bytes("swoje"));

    ought(!trouble(got.extracted({archive::Pick{"d", nook.where() / "d"}})).empty());

    static_cast<void>(got.extracted({archive::Pick{"d", nook.where() / "d"}}, fs::Conflict::overwrite).get());
    same(read(nook.where() / "d" / "b.txt"), std::string{"nowe b"});
    same(read(nook.where() / "d" / "c.txt"), std::string{"swoje"});

    static_cast<void>(got.extracted({archive::Pick{"d", nook.where() / "d"}}, fs::Conflict::rename).get());
    same(read(nook.where() / "d (2)" / "b.txt"), std::string{"nowe b"});
}

// Sizes and the offset past 32 bits come from ZIP64; a program before the archive shifts every
// offset; a name without the UTF-8 flag is UTF-8 when its bytes are, and code page 437 otherwise.
TRIAL("Archive: ZIP64, a program in front, and names in UTF-8 or code page 437") {
    const Nook nook;
    const stood::path wide = nook.where() / "wide.zip";

    Entry big{"duzy.bin", "dane"};

    big.wide = true;

    put(wide, zipped({big, Entry{"\x81" "ber.txt", "cp437"}, Entry{"\xC5\xBC\xC3\xB3\xC5\x82w.txt", "utf8"}}));

    const archive::Archive got = archive::opened(wide, {}).get();

    same(member(got, "duzy.bin").size, std::uintmax_t{4});
    ought(got.found(fs::from_string("\xC3\xBC" "ber.txt")).has_value());
    ought(got.found(fs::from_string("\xC5\xBC\xC3\xB3\xC5\x82w.txt")).has_value());

    static_cast<void>(got.extracted({archive::Pick{"duzy.bin", nook.where() / "duzy.bin"}}).get());
    same(read(nook.where() / "duzy.bin"), std::string{"dane"});

    const stood::path selfish = nook.where() / "program.zip";

    put(selfish, zipped({Entry{"w.txt", "w srodku"}}, "MZ to jest program, ktory rozpakowuje"));

    const archive::Archive inside = archive::opened(selfish, {}).get();

    static_cast<void>(inside.extracted({archive::Pick{"w.txt", nook.where() / "w.txt"}}).get());
    same(read(nook.where() / "w.txt"), std::string{"w srodku"});
}

// What is not a ZIP is said to be one, and a ZIP cut short is damaged.
TRIAL("Archive: bytes that are no ZIP, and a ZIP cut short, are refused") {
    const Nook nook;

    put(nook.where() / "nie.zip", talcite::bytes("to nie jest archiwum, tylko tekst o dlugosci ponad dwadziescia bajtow"));
    ought(trouble(archive::opened(nook.where() / "nie.zip", {})).find("Not a ZIP") != std::string::npos);

    put(nook.where() / "puste.zip", Bytes{});
    ought(trouble(archive::opened(nook.where() / "puste.zip", {})).find("Not a ZIP") != std::string::npos);

    Bytes whole = zipped({Entry{"a.txt", "aaaa"}});

    whole.erase(whole.begin() + 10, whole.begin() + 40);

    put(nook.where() / "uciety.zip", whole);
    ought(!trouble(archive::opened(nook.where() / "uciety.zip", {})).empty());

    put(nook.where() / "pusty.zip", zipped({}));
    ought(archive::opened(nook.where() / "pusty.zip", {}).get().members().empty());
}

// A tar walks its headers: files, folders, the ustar prefix, GNU long names, pax paths and times,
// links listed but refused; the data goes out in the archive's order.
TRIAL("Archive: a tar lists files, folders, long names and pax, and extracts them") {
    const Nook nook;
    const stood::path where = nook.where() / "kopia.tar";

    const std::string longest(150, 'n');

    Bytes made;

    tarred(made, Tarred{'5', "kopia/", "", 1'650'000'000, 0755});
    tarred(made, Tarred{'0', "kopia/a.txt", "pierwszy", 1'650'000'100, 0600});
    tarred(made, Tarred{'L', "././@LongLink", "kopia/" + longest + ".txt" + std::string(1, '\0')});
    tarred(made, Tarred{'0', "kopia/skrocona", "dlugi"});
    tarred(made, Tarred{'x', "PaxHeader", pax("path", "kopia/z pax \xC5\xBC.txt") + pax("mtime", "1234567890.25")});
    tarred(made, Tarred{'0', "kopia/niewazne", "pax"});
    tarred(made, Tarred{'2', "kopia/skrot", "", 1'650'000'000, 0777, "a.txt"});
    made.resize(made.size() + 1024, std::byte{0});

    put(where, made);

    const archive::Archive got = archive::opened(where, {}).get();

    same(names(got), (std::vector<std::string>{"kopia", "kopia/a.txt", "kopia/" + longest + ".txt", "kopia/z pax \xC5\xBC.txt", "kopia/skrot"}));
    ought(member(got, "kopia").kind == fs::Kind::folder);
    same(member(got, "kopia/a.txt").modified.time_since_epoch().count(), std::int64_t{1'650'000'100});
    ought(member(got, "kopia/a.txt").rights == stood::perms{0600});
    same(got.members()[3].modified.time_since_epoch().count(), std::int64_t{1'234'567'890});
    ought(member(got, "kopia/skrot").refusal == archive::Refusal::link);

    const std::vector<archive::Member> left = got.extracted({archive::Pick{"kopia", nook.where() / "wynik"}}).get();

    same(left.size(), std::size_t{1});
    same(read(nook.where() / "wynik" / "a.txt"), std::string{"pierwszy"});
    same(read(nook.where() / "wynik" / (longest + ".txt")), std::string{"dlugi"});
    same(read(nook.where() / "wynik" / fs::from_string("z pax \xC5\xBC.txt")), std::string{"pax"});
    same(fs::modified(nook.where() / "wynik").get().time_since_epoch().count(), std::int64_t{1'650'000'000});
}

// A tar whose header does not add up, one cut short, and something that is no tar at all.
TRIAL("Archive: a damaged tar header and a tar cut short are refused") {
    const Nook nook;

    Bytes bent = tar_of({Tarred{'0', "a.txt", "aaa"}, Tarred{'0', "b.txt", "bbb"}});

    bent[1024 + 5] = std::byte{'X'};

    put(nook.where() / "zgiety.tar", bent);
    ought(trouble(archive::opened(nook.where() / "zgiety.tar", {})).find("damaged") != std::string::npos);

    Bytes cut = tar_of({Tarred{'0', "a.txt", std::string(2000, 'a')}});

    cut.resize(1000);

    put(nook.where() / "uciety.tar", cut);
    ought(trouble(archive::opened(nook.where() / "uciety.tar", {})).find("cut short") != std::string::npos);

    put(nook.where() / "nie.tar", Bytes(512, std::byte{'z'}));
    ought(trouble(archive::opened(nook.where() / "nie.tar", {})).find("Not a tar") != std::string::npos);

    put(nook.where() / "pusty.tar", Bytes(1024, std::byte{0}));
    ought(archive::opened(nook.where() / "pusty.tar", {}).get().members().empty());
}

// A .tar.gz goes through the unpacker, there and again for the extraction; without one it does not
// open.
TRIAL("Archive: a tar.gz is read through the unpacker") {
    const Nook nook;
    const stood::path where = nook.where() / "kopia.tar.gz";

    put(where, tar_of({Tarred{'0', "a.txt", "przez gzip"}, Tarred{'0', "b.txt", "drugi"}}));

    const auto fed = std::make_shared<std::size_t>(0);
    const archive::Archive got = archive::opened(where, Seam<UnpackProvider>{std::make_shared<Passer>(fed)}).get();

    same(names(got), (std::vector<std::string>{"a.txt", "b.txt"}));
    ought(*fed > 0);

    const std::size_t listed = *fed;

    static_cast<void>(got.extracted({archive::Pick{"b.txt", nook.where() / "b.txt"}}).get());
    same(read(nook.where() / "b.txt"), std::string{"drugi"});
    ought(*fed > listed);

    ought(!trouble(archive::opened(where, {})).empty());
}

// Of two entries with one name the later counts, as unpackers have it: in the list once, with the
// later content.
TRIAL("Archive: of two entries with one name the later counts") {
    const Nook nook;
    const stood::path where = nook.where() / "dwa.zip";

    put(where, zipped({Entry{"a.txt", "pierwsza"}, Entry{"b.txt", "b"}, Entry{"a.txt", "druga"}}));

    const archive::Archive got = archive::opened(where, {}).get();

    same(names(got), (std::vector<std::string>{"a.txt", "b.txt"}));

    static_cast<void>(got.extracted({archive::Pick{"a.txt", nook.where() / "a.txt"}}).get());
    same(read(nook.where() / "a.txt"), std::string{"druga"});
}

// An extraction into a folder it makes itself, which fails on the way, takes back what it made:
// the folder does not stay half full. Into a folder that stood before, what came stays.
TRIAL("Archive: a failed extraction takes back the folder it made") {
    const Nook nook;
    const stood::path where = nook.where() / "polowa.zip";

    Entry wrong{"d/zly.txt", "zle"};

    wrong.crc = 1;

    put(where, zipped({Entry{"d/dobry.txt", "dobrze"}, wrong}));

    const archive::Archive got = archive::opened(where, {}).get();

    ought(!trouble(got.extracted({archive::Pick{{}, nook.where() / "nowy"}})).empty());
    ought(!stood::exists(nook.where() / "nowy"));

    stood::create_directories(nook.where() / "stary");

    ought(!trouble(got.extracted({archive::Pick{{}, nook.where() / "stary"}}, fs::Conflict::overwrite)).empty());
    same(read(nook.where() / "stary" / "d" / "dobry.txt"), std::string{"dobrze"});
    ought(!stood::exists(nook.where() / "stary" / "d" / "zly.txt"));
}

// A ZIP64 member whose packed size differs from its size: the extra field gives the size first,
// then the packed size, and each lands where it belongs.
TRIAL("Archive: a ZIP64 member's size and packed size come in the order of the format") {
    const Nook nook;
    const stood::path where = nook.where() / "wide.zip";

    Entry packed{"podwojny.txt", "abc"};

    packed.method = 8;
    packed.wide = true;
    packed.unpacked = "aabbcc";

    put(where, zipped({packed}));

    const archive::Archive got = archive::opened(where, Seam<UnpackProvider>{std::make_shared<Doubler>()}).get();

    same(member(got, "podwojny.txt").size, std::uintmax_t{6});
    same(member(got, "podwojny.txt").packed, std::uintmax_t{3});

    static_cast<void>(got.extracted({archive::Pick{"podwojny.txt", nook.where() / "podwojny.txt"}}).get());
    same(read(nook.where() / "podwojny.txt"), std::string{"aabbcc"});
}

// The trick of a zip bomb that needs no nesting: many entries of the central directory read one
// packed kernel. Members whose data overlap are refused at the opening, whole.
TRIAL("Archive: members that share their data are refused, as a zip bomb's do") {
    const Nook nook;
    const stood::path where = nook.where() / "bomba.zip";

    Entry second{"b.txt", "bbbb"};

    second.at = 0;

    put(where, zipped({Entry{"a.txt", "aaaa"}, second}));

    ought(trouble(archive::opened(where, {})).find("overlap") != std::string::npos);

    put(where, zipped({Entry{"a.txt", "aaaa"}, Entry{"b.txt", "bbbb"}}));

    same(names(archive::opened(where, {}).get()), (std::vector<std::string>{"a.txt", "b.txt"}));
}

// A central directory may claim any count of entries; past a million the archive is refused before
// a single entry is read into memory.
TRIAL("Archive: more than a million members are refused") {
    const Nook nook;
    const stood::path where = nook.where() / "tlum.zip";

    Bytes made;

    le(made, 0x06064b50, 4);
    le(made, 44, 8);
    le(made, 45, 2);
    le(made, 45, 2);
    le(made, 0, 4);
    le(made, 0, 4);
    le(made, 2'000'000, 8);
    le(made, 2'000'000, 8);
    le(made, 0, 8);
    le(made, 0, 8);

    le(made, 0x07064b50, 4);
    le(made, 0, 4);
    le(made, 0, 8);
    le(made, 1, 4);

    le(made, 0x06054b50, 4);
    le(made, 0, 2);
    le(made, 0, 2);
    le(made, 0xFFFF, 2);
    le(made, 0xFFFF, 2);
    le(made, 0xFFFFFFFFU, 4);
    le(made, 0xFFFFFFFFU, 4);
    le(made, 0, 2);

    put(where, made);

    ought(trouble(archive::opened(where, {})).find("too many members") != std::string::npos);
}

// No member comes out larger than it declares, so what the members declare is what an extraction
// can cost; one that declares more than the target's free space is refused before the first byte.
TRIAL("Archive: an extraction larger than the free space is refused before it writes") {
    const Nook nook;
    const stood::path where = nook.where() / "wielka.zip";

    Entry huge{"zera.bin", "0000"};

    huge.wide = true;
    huge.declared = std::uint64_t{1} << 60;

    put(where, zipped({huge}));

    const archive::Archive got = archive::opened(where, {}).get();

    same(member(got, "zera.bin").size, std::uintmax_t{std::uint64_t{1} << 60});

    ought(trouble(got.extracted({archive::Pick{{}, nook.where() / "cel"}})).find("free space") != std::string::npos);
    ought(!stood::exists(nook.where() / "cel"));
    ought(!stood::exists(nook.where() / "zera.bin"));
    ought(!trouble(got.extracted({archive::Pick{"zera.bin", nook.where() / "zera.bin"}})).empty());
    ought(!stood::exists(nook.where() / "zera.bin"));
}

// A tar.gz has no table of contents, so its opening inflates the whole stream; a stream that swells
// without end stops as soon as nobody waits for the opening any more.
TRIAL("Archive: opening a tar.gz stops when it is no longer wanted") {
    const Nook nook;
    const stood::path where = nook.where() / "bomba.tar.gz";

    Tarred zeros{'0', "zera.bin", ""};

    zeros.told = 077777777777ULL;

    Bytes made;

    tarred(made, zeros);

    made.resize(4 * 1024 * 1024, std::byte{'x'});

    put(where, made);

    const auto fed = std::make_shared<std::atomic<std::size_t>>(0);

    {
        std::optional<Promise<archive::Archive>> opening{archive::opened(where, Seam<UnpackProvider>{std::make_shared<Sweller>(fed)})};

        for (int tries = 0; tries < 2000 && fed->load() < 3; ++tries) std::this_thread::sleep_for(std::chrono::milliseconds{1});

        opening.reset();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    const std::size_t then = fed->load();

    std::this_thread::sleep_for(std::chrono::milliseconds{150});

    ought(fed->load() <= then + 1);
    ought(fed->load() < 200);
}
