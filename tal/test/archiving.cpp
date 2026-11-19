#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tal/archive.hpp"
#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "archiving.hpp"
#include "inflating.hpp"

// Archives: names laid out safely, zip and tar (plain and through gzip) written here byte by byte
// for each thing their readers must understand or refuse, and extraction onto the disk of a
// folder of the trials: whole files or nothing, conflicts, refusals, times and rights, progress.

namespace {

using tal::Bytes;
using tal::Kind;
using tal::Path;
using tal::archive::Archive;
using tal::archive::Format;
using tal::archive::Member;
using tal::archive::Pick;
using tal::archive::Refusal;

using namespace std::chrono_literals;

// The kind of trouble `work` ended in, or "none".
template <typename T>
std::string kind_of(tal::Loop& loop, tal::Task<T> work) {
    try {
        (void)tal::wait(loop, std::move(work));
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())};
    }

    return "none";
}

// What reading `work` threw: its kind and message, or "none".
template <typename T>
std::string refusal_of(tal::Loop& loop, tal::Task<T> work) {
    try {
        (void)tal::wait(loop, std::move(work));
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())} + ": " + why.what();
    }

    return "none";
}

// The kind `kind` as spoken.
std::string named(Kind kind) { return std::string{tal::spoken(kind)}; }

// Appends the `much` low bytes of `value` to `into`, the lowest first.
void put(Bytes& into, std::uint64_t value, int much) {
    for (int one = 0; one < much; ++one) into.push_back(static_cast<std::byte>((value >> (8 * one)) & 0xFF));
}

// Appends `text` to `into`.
void put(Bytes& into, std::string_view text) {
    const Bytes made = tal::bytes(text);

    into.insert(into.end(), made.begin(), made.end());
}

// `text` in raw deflate's fixed code, each byte a literal.
Bytes fixed(std::string_view text) {
    Bytes made;
    int used = 0;

    const auto bit = [&](unsigned one) {
        if (used == 0) made.push_back(std::byte{0});

        made.back() |= static_cast<std::byte>(one << used);
        used = (used + 1) % 8;
    };
    const auto code = [&](unsigned value, int length) {
        for (int one = length - 1; one >= 0; --one) bit((value >> one) & 1U);
    };

    bit(1);
    bit(1);
    bit(0);

    for (const char one : text) {
        const auto symbol = static_cast<unsigned char>(one);

        if (symbol < 144) code(0x30U + symbol, 8);
        else code(0x190U + symbol - 144U, 9);
    }

    code(0, 7);

    return made;
}

// One entry of a zip written here: its name and data, then what differs from a plain stored file
// made on Unix.
struct Entry {
    // A stored file `name` holding `data`.
    Entry(std::string name_given, std::string data_given) : name(std::move(name_given)), data(std::move(data_given)) {}

    // Deflated in the fixed code.
    Entry& deflate() {
        deflated = true;

        return *this;
    }

    // Made by `host` (the high byte) and version.
    Entry& by(std::uint16_t host) {
        made_by = host;

        return *this;
    }

    // With the flags `given`.
    Entry& flagged(std::uint16_t given) {
        flags = given;

        return *this;
    }

    // With the method `given`.
    Entry& packed_by(std::uint16_t given) {
        method = given;

        return *this;
    }

    // With the external attributes `given`.
    Entry& attributes(std::uint32_t given) {
        outer = given;

        return *this;
    }

    // With the extra field `given`.
    Entry& with(Bytes given) {
        extra = std::move(given);

        return *this;
    }

    // Saying the CRC-32 `given`.
    Entry& summed(std::uint32_t given) {
        crc = given;

        return *this;
    }

    // Saying the size `given`.
    Entry& sized(std::uint64_t given) {
        size = given;

        return *this;
    }

    // With its sizes and offset in ZIP64's extra field.
    Entry& wide() {
        zip64 = true;

        return *this;
    }

    // With `given` as its packed bytes, whatever they say.
    Entry& packed_as(Bytes given) {
        raw = std::move(given);

        return *this;
    }

    // At the DOS time `time` of the DOS date `date`.
    Entry& dated(std::uint16_t time_given, std::uint16_t date_given) {
        time = time_given;
        date = date_given;

        return *this;
    }

    std::string name;
    std::string data;
    bool deflated = false;
    std::uint16_t made_by = 0x0314;
    std::uint16_t flags = 0;
    std::optional<std::uint16_t> method;
    std::uint32_t outer = 0100644U << 16;
    Bytes extra;
    std::optional<std::uint32_t> crc;
    std::optional<std::uint64_t> size;
    bool zip64 = false;
    std::optional<Bytes> raw;
    std::uint16_t time = 0x6000;
    std::uint16_t date = 0x5A21;
};

// A zip of `entries` after `before`, its directory's count `count` when given, with ZIP64's end
// records when `zip64_end` (of which the ordinary end leaves only the offset full when
// `offset_alone`), and the archive's `comment`.
Bytes zipped(const std::vector<Entry>& entries, const std::string& before = {}, std::optional<std::uint64_t> count = {}, bool zip64_end = false,
             const std::string& comment = "comment", bool offset_alone = false) {
    Bytes made = tal::bytes(before);
    Bytes directory;

    for (const Entry& one : entries) {
        const Bytes data = one.raw ? *one.raw : one.deflated ? fixed(one.data) : tal::bytes(one.data);
        const std::uint64_t at = made.size() - before.size();
        const std::uint32_t crc = one.crc.value_or(tal::inflating::crc32(0, tal::bytes(one.data)));
        const std::uint64_t size = one.size.value_or(one.data.size());
        const std::uint16_t method = one.method.value_or(one.deflated ? 8 : 0);
        Bytes extra = one.extra;

        if (one.zip64) {
            put(extra, 0x0001, 2);
            put(extra, 24, 2);
            put(extra, size, 8);
            put(extra, data.size(), 8);
            put(extra, at, 8);
        }

        put(made, 0x04034b50, 4);
        put(made, 20, 2);
        put(made, one.flags, 2);
        put(made, method, 2);
        put(made, one.time, 2);
        put(made, one.date, 2);
        put(made, crc, 4);
        put(made, data.size(), 4);
        put(made, size, 4);
        put(made, one.name.size(), 2);
        put(made, one.extra.size(), 2);
        put(made, one.name);
        made.insert(made.end(), one.extra.begin(), one.extra.end());
        made.insert(made.end(), data.begin(), data.end());

        put(directory, 0x02014b50, 4);
        put(directory, one.made_by, 2);
        put(directory, 20, 2);
        put(directory, one.flags, 2);
        put(directory, method, 2);
        put(directory, one.time, 2);
        put(directory, one.date, 2);
        put(directory, crc, 4);
        put(directory, one.zip64 ? 0xFFFFFFFFU : data.size(), 4);
        put(directory, one.zip64 ? 0xFFFFFFFFU : size, 4);
        put(directory, one.name.size(), 2);
        put(directory, extra.size(), 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, one.outer, 4);
        put(directory, one.zip64 ? 0xFFFFFFFFU : at, 4);
        put(directory, one.name);
        directory.insert(directory.end(), extra.begin(), extra.end());
    }

    const std::uint64_t offset = made.size() - before.size();
    const std::uint64_t entries_count = count.value_or(entries.size());

    made.insert(made.end(), directory.begin(), directory.end());

    if (zip64_end) {
        const std::uint64_t end64_at = made.size() - before.size();

        put(made, 0x06064b50, 4);
        put(made, 44, 8);
        put(made, 45, 2);
        put(made, 45, 2);
        put(made, 0, 4);
        put(made, 0, 4);
        put(made, entries_count, 8);
        put(made, entries_count, 8);
        put(made, directory.size(), 8);
        put(made, offset, 8);
        put(made, 0x07064b50, 4);
        put(made, 0, 4);
        put(made, end64_at + before.size(), 8);
        put(made, 1, 4);
    }

    put(made, 0x06054b50, 4);
    put(made, 0, 4);
    put(made, zip64_end && !offset_alone ? 0xFFFF : entries_count, 2);
    put(made, zip64_end && !offset_alone ? 0xFFFF : entries_count, 2);
    put(made, directory.size(), 4);
    put(made, zip64_end ? 0xFFFFFFFFU : offset, 4);
    put(made, comment.size(), 2);
    put(made, comment);

    return made;
}

// One header and data of a tar written here: its name, type and data, then what differs.
struct Record {
    // The record `name` of `type` holding `data`.
    Record(std::string name_given, char type_given, std::string data_given) : name(std::move(name_given)), type(type_given), data(std::move(data_given)) {}

    // With the mode `given`.
    Record& moded(std::uint32_t given) {
        mode = given;

        return *this;
    }

    // At the time `given`.
    Record& timed(std::uint64_t given) {
        mtime = given;

        return *this;
    }

    // With the ustar prefix `given`.
    Record& prefixed(std::string given) {
        prefix = std::move(given);

        return *this;
    }

    // Saying the size `given`, in base-256 when `binary`.
    Record& sized(std::uint64_t given, bool binary = false) {
        size = given;
        base256 = binary;

        return *this;
    }

    // With the raw 12 bytes `given` as its size field.
    Record& size_field(std::string given) {
        raw_size = std::move(given);

        return *this;
    }

    // With the raw bytes `given` at `at` of its header, written last.
    Record& overwritten(std::size_t at, std::string given) {
        raw.emplace_back(at, std::move(given));

        return *this;
    }

    // Its checksum counted signed, over a byte past 127.
    Record& signed_up() {
        signed_sum = true;

        return *this;
    }

    // Its checksum one too high.
    Record& miscounted() {
        broken_sum = true;

        return *this;
    }

    std::string name;
    char type = '0';
    std::string data;
    std::uint32_t mode = 0644;
    std::uint64_t mtime = 1700000000;
    std::string prefix;
    std::optional<std::uint64_t> size;
    bool base256 = false;
    bool signed_sum = false;
    bool broken_sum = false;
    std::optional<std::string> raw_size;
    std::vector<std::pair<std::size_t, std::string>> raw;
};

// The octal digits of `value` in a field of `much` bytes, ending in a zero.
std::string octal(std::uint64_t value, std::size_t much) {
    std::string made(much - 1, '0');

    for (std::size_t at = much - 1; at-- > 0 && value != 0; value /= 8) made[at] = static_cast<char>('0' + value % 8);

    return made + '\0';
}

// A tar of `records`, ended by two blocks of zeros unless `ended` is false.
Bytes tarred(const std::vector<Record>& records, bool ended = true) {
    Bytes made;

    for (const Record& one : records) {
        if (one.type == 'Z') {
            made.resize(made.size() + 512);

            continue;
        }

        std::array<std::byte, 512> header{};
        const auto write = [&header](std::size_t at, std::string_view text) {
            for (std::size_t one_at = 0; one_at < text.size(); ++one_at) header[at + one_at] = static_cast<std::byte>(text[one_at]);
        };
        const std::uint64_t size = one.size.value_or(one.data.size());

        write(0, one.name.substr(0, 100));
        write(100, octal(one.mode, 8));
        write(108, octal(0, 8));
        write(116, octal(0, 8));

        if (one.base256) {
            header[124] = std::byte{0x80};

            for (int at = 0; at < 8; ++at) header[135 - static_cast<std::size_t>(at)] = static_cast<std::byte>((size >> (8 * at)) & 0xFF);
        } else {
            write(124, octal(size, 12));
        }

        if (one.raw_size) write(124, *one.raw_size);

        write(136, octal(one.mtime, 12));
        write(148, "        ");
        header[156] = static_cast<std::byte>(one.type);
        write(257, std::string_view{"ustar\0" "00", 8});
        write(345, one.prefix);

        if (one.signed_sum) header[400] = std::byte{0xC8};

        for (const auto& [at, bytes] : one.raw) write(at, bytes);

        std::int64_t sum = 0;

        for (const std::byte each : header) sum += one.signed_sum ? static_cast<std::int8_t>(each) : std::to_integer<int>(each);

        write(148, octal(static_cast<std::uint64_t>(sum) + (one.broken_sum ? 1 : 0), 7));
        made.insert(made.end(), header.begin(), header.end());
        put(made, one.data);
        made.resize((made.size() + 511) / 512 * 512);
    }

    if (ended) made.resize(made.size() + 1024);

    return made;
}

// A pax extended header saying `records`, each "key=value".
Record pax(const std::vector<std::string>& records) {
    Record made{"PaxHeader", 'x', ""};

    for (const std::string& one : records) {
        std::size_t length = one.size() + 3;

        while (std::to_string(length).size() + one.size() + 2 != length) ++length;

        made.data += std::to_string(length) + " " + one + "\n";
    }

    return made;
}

// `plain` in a gzip member whose deflate is one stored block.
Bytes gzipped(const Bytes& plain) {
    Bytes made{std::byte{0x1F}, std::byte{0x8B}, std::byte{8}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{3}};

    for (std::size_t at = 0; at < plain.size() || at == 0; at += 65535) {
        const std::size_t much = std::min<std::size_t>(65535, plain.size() - at);

        made.push_back(at + much >= plain.size() ? std::byte{1} : std::byte{0});
        put(made, much, 2);
        put(made, much ^ 0xFFFF, 2);
        made.insert(made.end(), plain.begin() + static_cast<std::ptrdiff_t>(at), plain.begin() + static_cast<std::ptrdiff_t>(at + much));
    }

    put(made, tal::inflating::crc32(0, plain), 4);
    put(made, plain.size(), 4);

    return made;
}

// A folder of a trial in the system's temporary folder, removed after.
class Desk {
public:
    // Makes the folder.
    Desk() {
        static std::atomic<int> numbered{0};

        root_ = std::filesystem::temp_directory_path() /
                ("tal-archives-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++numbered));
        std::filesystem::create_directories(root_);
    }

    // Removes the folder.
    ~Desk() {
        std::error_code ignored;

        std::filesystem::remove_all(root_, ignored);
    }

    Desk(const Desk&) = delete;
    Desk& operator=(const Desk&) = delete;

    // The path of `name` in the folder.
    [[nodiscard]] Path at(const std::string& name) const {
        const std::u8string spelled = (std::filesystem::canonical(root_) / std::filesystem::path{std::u8string{name.begin(), name.end()}}).generic_u8string();

        return Path{std::string{spelled.begin(), spelled.end()}};
    }

    // Writes `bytes` to the file `name`.
    void put(const std::string& name, const Bytes& bytes) const {
        std::ofstream out{root_ / name, std::ios::binary};

        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // The text of the file `name`, or "none" when there is none.
    [[nodiscard]] std::string got(const std::string& name) const {
        if (!std::filesystem::is_regular_file(root_ / std::filesystem::path{std::u8string{name.begin(), name.end()}})) return "none";

        std::ifstream in{root_ / std::filesystem::path{std::u8string{name.begin(), name.end()}}, std::ios::binary};

        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    // The names in the folder `name`, sorted.
    [[nodiscard]] std::vector<std::string> listed(const std::string& name = {}) const {
        std::vector<std::string> made;

        for (const auto& one : std::filesystem::directory_iterator{root_ / name}) {
            const std::u8string spelled = one.path().filename().u8string();

            made.emplace_back(spelled.begin(), spelled.end());
        }

        std::sort(made.begin(), made.end());

        return made;
    }

    // The modification time of `name` in whole seconds since 1970.
    [[nodiscard]] std::int64_t modified(const std::string& name) const {
        const auto when = std::filesystem::last_write_time(root_ / name);

        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(when).time_since_epoch()).count();
    }

    // The rights of `name`.
    [[nodiscard]] unsigned rights(const std::string& name) const { return static_cast<unsigned>(std::filesystem::status(root_ / name).permissions() & std::filesystem::perms::all); }

private:
    std::filesystem::path root_;
};

// The member names of `archive`, in its order, with a mark for each type and refusal.
std::string shown(const Archive& archive) {
    std::string made;

    for (const Member& one : archive.members()) {
        made += (made.empty() ? "" : " ") + std::string{one.name.stored()};

        if (one.type == tal::fs::Type::folder) made += "/";

        if (one.refusal == Refusal::unsafe) made += "!unsafe";
        if (one.refusal == Refusal::link) made += "!link";
        if (one.refusal == Refusal::encrypted) made += "!encrypted";
        if (one.refusal == Refusal::method) made += "!method";
    }

    return made;
}

// The member `name` of `archive`.
const Member& member(const Archive& archive, const std::string& name) { return archive.members()[archive.found(Path{name}).value()]; }

}

TRIAL("Archive: names are laid out within the archive; what leads out or holds control characters is unsafe") {
    using tal::archive::archiving::laid;

    const auto shown_laid = [](std::string_view stored) {
        const tal::archive::archiving::Laid one = laid(stored);

        return std::string{one.name.stored()} + (one.unsafe ? " unsafe" : "");
    };

    SAME(shown_laid("films/kino.mkv"), std::string{"films/kino.mkv"});
    SAME(shown_laid("films\\kino.mkv"), std::string{"films/kino.mkv"});
    SAME(shown_laid("./films//./kino/"), std::string{"films/kino"});
    SAME(shown_laid("/etc/passwd"), std::string{"etc/passwd unsafe"});
    SAME(shown_laid("\\etc"), std::string{"etc unsafe"});
    SAME(shown_laid("../evil"), std::string{"evil unsafe"});
    SAME(shown_laid("a/../../b"), std::string{"a/b unsafe"});
    SAME(shown_laid("..."), std::string{"..."});
    SAME(shown_laid("C:/x"), std::string{"C:/x unsafe"});
    SAME(shown_laid("a:stream"), std::string{"a:stream unsafe"});
    SAME(shown_laid("a\x01" "b"), std::string{"a\x01" "b unsafe"});
    SAME(shown_laid("a\x7F"), std::string{"a\x7F unsafe"});
    SAME(shown_laid("a\x1F"), std::string{"a\x1F unsafe"});
    SAME(shown_laid(" a "), std::string{" a "});
    SAME(shown_laid(""), std::string{});

    SAME(tal::archive::archiving::from_cp437("a\x7F\x80\xE1\xFF"), std::string{"a\x7F\xC3\x87\xC3\x9F\xC2\xA0"});
    SAME(tal::archive::archiving::from_latin1("a\xE9\xFF"), std::string{"a\xC3\xA9\xC3\xBF"});

    SAME(tal::archive::format_of(Path{"/x/A.ZIP"}).value_or(Format::tar), Format::zip);
    SAME(tal::archive::format_of(Path{"a.tar"}).value_or(Format::zip), Format::tar);
    SAME(tal::archive::format_of(Path{"a.Tar.Gz"}).value_or(Format::zip), Format::tar_gzip);
    SAME(tal::archive::format_of(Path{"a.tgz"}).value_or(Format::zip), Format::tar_gzip);
    OUGHT(!tal::archive::format_of(Path{".zip"}));
    OUGHT(!tal::archive::format_of(Path{"a.gz"}));
    OUGHT(!tal::archive::format_of(Path{"a.zip.txt"}));
    OUGHT(!tal::archive::format_of(Path{"zip"}));
}

TRIAL("Archive: room and FAT before the first byte; times held within the clock's reach") {
    tal::fs::Volume volume;

    volume.free = 1000;
    volume.format = "ext4";

    tal::archive::archiving::fits(volume, 1000, std::uint64_t{1} << 40, Path{"/x"});
    THREW(tal::archive::archiving::fits(volume, 1001, 1, Path{"/x"}));

    volume.free = std::uint64_t{1} << 40;

    for (const char* fat : {"vfat", "FAT32", "fat", "msdos"}) {
        volume.format = fat;
        tal::archive::archiving::fits(volume, 1, (std::uint64_t{1} << 32) - 1, Path{"/x"});

        try {
            tal::archive::archiving::fits(volume, 1, std::uint64_t{1} << 32, Path{"/x"});
            OUGHT(false);
        } catch (const tal::Trouble& why) {
            SAME(named(why.kind()), named(Kind::unsupported));
        }
    }

    try {
        volume.free = 5;
        tal::archive::archiving::fits(volume, 6, 1, Path{"/x"});
        OUGHT(false);
    } catch (const tal::Trouble& why) {
        SAME(named(why.kind()), named(Kind::full));
    }

    using tal::archive::archiving::instant;

    SAME(instant(5).time_since_epoch(), std::chrono::system_clock::duration{5s});
    SAME(instant(-5).time_since_epoch(), std::chrono::system_clock::duration{-5s});
    SAME(instant(INT64_MAX).time_since_epoch(), std::chrono::system_clock::duration{std::chrono::seconds{std::int64_t{1} << 33}});
    SAME(instant(INT64_MIN).time_since_epoch(), std::chrono::system_clock::duration{std::chrono::seconds{-(std::int64_t{1} << 33)}});
}

TRIAL("Zip: the directory after a program, names of UTF-8, CP437 and Unicode paths, folders, refusals, ZIP64") {
    tal::Loop loop;
    const Desk desk;
    Bytes unicode;

    put(unicode, 0x7075, 2);
    put(unicode, 5 + 9, 2);
    put(unicode, 1, 1);
    put(unicode, tal::inflating::crc32(0, tal::bytes("\x87\x61.txt")), 4);
    put(unicode, "\xC3\xA7" "a.txt\xC3\xA9");

    Bytes stale;

    put(stale, 0x7075, 2);
    put(stale, 5 + 5, 2);
    put(stale, 1, 1);
    put(stale, 1234, 4);
    put(stale, "wrong");

    Bytes stamp;

    put(stamp, 0x5455, 2);
    put(stamp, 5, 2);
    put(stamp, 1, 1);
    put(stamp, 1600000000, 4);

    desk.put("a.zip", zipped({Entry{"films/", ""}.attributes(0040755U << 16),
                              Entry{"films/kino.txt", "Poirot"}.attributes(0100640U << 16).with(stamp),
                              Entry{"deflated.txt", "Hastings and Japp"}.deflate(),
                              Entry{"implied/deep/x.txt", "x"}.by(0x0014).attributes(0),
                              Entry{"dos/", ""}.by(0x0014).attributes(0x10),
                              Entry{"z\xC3\xB3\xC5\x82w.txt", "zolw"}.by(0x0014),
                              Entry{"\x87\x61.txt", "u"}.by(0x0014).attributes(0).with(unicode),
                              Entry{"\x87\x62.txt", "v"}.by(0x0014).attributes(0).with(stale),
                              Entry{"link", "films"}.attributes(0120777U << 16),
                              Entry{"secret.txt", "s"}.flagged(1),
                              Entry{"aes.txt", "s"}.packed_by(99),
                              Entry{"bzip.txt", "s"}.packed_by(12),
                              Entry{"../evil.txt", "e"},
                              Entry{"films/kino.txt", "Poirot again"},
                              Entry{"big.txt", "zip64"}.wide()},
                             "#!/bin/sh\necho a self-extracting program\n"));

    const Archive archive = tal::wait(loop, tal::archive::opened(desk.at("a.zip")));

    OUGHT(archive.held());
    SAME(archive.format(), Format::zip);
    SAME(archive.where(), desk.at("a.zip"));
    SAME(shown(archive), std::string{"films/ films/kino.txt deflated.txt implied/deep/x.txt dos/ z\xC3\xB3\xC5\x82w.txt \xC3\xA7" "a.txt\xC3\xA9 \xC3\xA7" "b.txt link!link "
                                     "secret.txt!encrypted aes.txt!encrypted bzip.txt!method evil.txt!unsafe big.txt implied/deep/ implied/"});
    SAME(member(archive, "films/kino.txt").size, std::uint64_t{12});
    SAME(member(archive, "films/kino.txt").stored, std::string{"films/kino.txt"});
    SAME(member(archive, "films/kino.txt").rights.value_or(0), std::uint16_t{0644});
    SAME(member(archive, "films").rights.value_or(0), std::uint16_t{0755});
    SAME(member(archive, "deflated.txt").size, std::uint64_t{17});
    OUGHT(member(archive, "deflated.txt").packed != 17);
    OUGHT(!member(archive, "implied/deep/x.txt").rights);
    OUGHT(member(archive, "big.txt").size == 5 && member(archive, "big.txt").packed == 5);
    SAME(member(archive, "evil.txt").stored, std::string{"../evil.txt"});

    std::vector<std::string> root;

    for (const std::size_t one : archive.within(Path{})) root.emplace_back(archive.members()[one].name.stored());

    SAME(root, (std::vector<std::string>{"films", "deflated.txt", "dos", "z\xC3\xB3\xC5\x82w.txt", "\xC3\xA7" "a.txt\xC3\xA9", "\xC3\xA7" "b.txt", "link", "secret.txt", "aes.txt",
                                         "bzip.txt", "evil.txt", "big.txt", "implied"}));
    SAME(archive.within(Path{"films"}).size(), std::size_t{1});
    OUGHT(!archive.found(Path{"nothing"}));

    desk.put("b.zip", zipped({Entry{"a.txt", "a"}.with(stamp)}));

    const Archive dated = tal::wait(loop, tal::archive::opened(desk.at("b.zip")));

    SAME(std::chrono::duration_cast<std::chrono::seconds>(dated.members()[0].modified.time_since_epoch()).count(), std::int64_t{1600000000});

    desk.put("c.zip", zipped({Entry{"a.txt", "a"}.dated(0x0000, 0x0000), Entry{"b.txt", "b"}.dated(0xBF7D, 0x5A21)}));

    const Archive dos = tal::wait(loop, tal::archive::opened(desk.at("c.zip")));

    SAME(std::chrono::duration_cast<std::chrono::seconds>(dos.members()[0].modified.time_since_epoch()).count(), std::int64_t{315532800});

    std::tm local{};
    const std::time_t was = std::chrono::system_clock::to_time_t(dos.members()[1].modified);

#ifdef _WIN32
    (void)localtime_s(&local, &was);
#else
    (void)localtime_r(&was, &local);
#endif

    SAME(local.tm_year, 125);
    SAME(local.tm_mon, 0);
    SAME(local.tm_mday, 1);
    SAME(local.tm_hour, 23);
    SAME(local.tm_min, 59);
    SAME(local.tm_sec, 58);

    desk.put("zip64.zip", zipped({Entry{"a.txt", "abc"}, Entry{"b.txt", "de"}.wide()}, "", {}, true));

    const Archive wide = tal::wait(loop, tal::archive::opened(desk.at("zip64.zip")));

    SAME(shown(wide), std::string{"a.txt b.txt"});
    SAME(wide.members()[1].size, std::uint64_t{2});
}

TRIAL("Zip: what is not a zip, a directory outside the file, members that overlap, too many: each refused at opening") {
    tal::Loop loop;
    const Desk desk;

    desk.put("empty.zip", Bytes{});
    desk.put("text.zip", tal::bytes(std::string(100, 'x')));

    SAME(refusal_of(loop, tal::archive::opened(desk.at("empty.zip"))), std::string{"malformed: Not a zip archive."});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("text.zip"))), std::string{"malformed: Not a zip archive."});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("none.zip"))).substr(0, 8), std::string{"missing:"});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("a.rar"))), std::string{"unsupported: Not an archive tal reads."});

    Bytes outside = zipped({Entry{"a.txt", "a"}});

    outside[outside.size() - 7 - 2 - 4 - 1] = std::byte{0x7F};
    desk.put("outside.zip", outside);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("outside.zip"))), std::string{"malformed: The central directory lies outside the file."});

    const std::array<std::byte, 4> central{std::byte{0x50}, std::byte{0x4B}, std::byte{1}, std::byte{2}};
    Bytes broken = zipped({Entry{"a.txt", "a"}});
    const auto directory = static_cast<std::size_t>(std::search(broken.begin(), broken.end(), central.begin(), central.end()) - broken.begin());

    broken[directory] = std::byte{0};
    desk.put("broken.zip", broken);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("broken.zip"))), std::string{"malformed: The central directory is damaged."});

    desk.put("count.zip", zipped({Entry{"a.txt", "a"}}, "", 2));
    SAME(refusal_of(loop, tal::archive::opened(desk.at("count.zip"))), std::string{"malformed: The central directory is damaged."});

    desk.put("many.zip", zipped({Entry{"a.txt", "a"}}, "", 1000001, true));
    SAME(refusal_of(loop, tal::archive::opened(desk.at("many.zip"))), std::string{"malformed: The archive holds too many members."});

    Bytes bomb = zipped({Entry{"a.txt", "aaaa"}, Entry{"b.txt", "bbbb"}});
    const auto first = std::search(bomb.begin(), bomb.end(), central.begin(), central.end());
    const auto second = static_cast<std::size_t>(std::search(first + 1, bomb.end(), central.begin(), central.end()) - bomb.begin());

    for (std::size_t at = 0; at < 4; ++at) bomb[second + 42 + at] = std::byte{0};

    desk.put("bomb.zip", bomb);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("bomb.zip"))), std::string{"malformed: Members of the zip archive overlap, as in a zip bomb."});

    Bytes cut = zipped({Entry{"a.txt", "a"}});

    cut.resize(cut.size() - 12);
    desk.put("cut.zip", cut);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("cut.zip"))), std::string{"malformed: Not a zip archive."});

    Bytes locator = zipped({Entry{"a.txt", "a"}}, "", {}, true);

    locator[locator.size() - 22 - 7 - 20] = std::byte{0};
    desk.put("locator.zip", locator);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("locator.zip"))), std::string{"malformed: The ZIP64 locator is missing."});

    Bytes end64 = zipped({Entry{"a.txt", "a"}}, "", {}, true);

    end64[end64.size() - 22 - 7 - 20 - 56] = std::byte{0};
    desk.put("end64.zip", end64);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("end64.zip"))), std::string{"malformed: The ZIP64 end record is damaged."});

    Bytes far = zipped({Entry{"a.txt", "a"}}, "", {}, true);

    put(far, 0, 0);
    far[far.size() - 22 - 7 - 20 + 8 + 7] = std::byte{0x10};
    desk.put("far.zip", far);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("far.zip"))), std::string{"malformed: The ZIP64 end record lies outside the file."});
}

TRIAL("Zip: extraction writes whole files with their times and rights, skips refusals, and leaves nothing of a damaged one") {
    tal::Loop loop;
    const Desk desk;

    Bytes stamp;

    put(stamp, 0x5455, 2);
    put(stamp, 5, 2);
    put(stamp, 1, 1);
    put(stamp, 1600000000, 4);

    desk.put("a.zip", zipped({Entry{"deflated.txt", "Hastings and Japp"}.deflate(),
                              Entry{"films/", ""}.attributes(0040750U << 16),
                              Entry{"films/kino.txt", "Poirot"}.attributes(0100600U << 16).with(stamp),
                              Entry{"implied/x.txt", "x"},
                              Entry{"link", "films"}.attributes(0120777U << 16),
                              Entry{"../evil.txt", "e"}}));

    const Archive archive = tal::wait(loop, tal::archive::opened(desk.at("a.zip")));
    tal::Progress progress;
    const std::vector<Member> refused = tal::wait(loop, archive.extracted({Pick{Path{}, desk.at("out")}}, tal::fs::Conflict::fail, &progress));

    SAME(refused.size(), std::size_t{2});
    SAME(desk.listed("out"), (std::vector<std::string>{"deflated.txt", "films", "implied"}));
    SAME(desk.got("out/films/kino.txt"), std::string{"Poirot"});
    SAME(desk.got("out/deflated.txt"), std::string{"Hastings and Japp"});
    SAME(desk.got("out/implied/x.txt"), std::string{"x"});
    SAME(progress.total(), std::uint64_t{24});
    SAME(progress.done(), std::uint64_t{24});

    std::tm local{};
    const std::time_t was = std::chrono::system_clock::to_time_t(member(archive, "deflated.txt").modified);

#ifdef _WIN32
    (void)localtime_s(&local, &was);
#else
    (void)localtime_r(&was, &local);
    SAME(desk.rights("out/films/kino.txt"), 0600U);
    SAME(desk.rights("out/films"), 0750U);
#endif

    SAME(desk.modified("out/deflated.txt"), static_cast<std::int64_t>(was));
    SAME(desk.modified("out/films"), static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(member(archive, "films").modified)));

    const std::vector<Member> one = tal::wait(loop, archive.extracted({Pick{Path{"films/kino.txt"}, desk.at("single.txt")}, Pick{Path{"films"}, desk.at("f")}}));

    SAME(one.size(), std::size_t{0});
    SAME(desk.got("single.txt"), std::string{"Poirot"});
    SAME(desk.got("f/kino.txt"), std::string{"Poirot"});
    SAME(tal::wait(loop, archive.extracted({Pick{Path{"link"}, desk.at("l")}})).size(), std::size_t{1});
    SAME(desk.got("l"), std::string{"none"});
    SAME(kind_of(loop, archive.extracted({Pick{Path{"nothing"}, desk.at("n")}})), named(Kind::missing));

    desk.put("crc.zip", zipped({Entry{"a/one.txt", "good"}, Entry{"a/two.txt", "damaged"}.summed(1234)}));
    desk.put("short.zip", zipped({Entry{"a/one.txt", "good"}.deflate().sized(10)}));
    desk.put("long.zip", zipped({Entry{"a/one.txt", "good"}.deflate().sized(3)}));

    const Archive crc = tal::wait(loop, tal::archive::opened(desk.at("crc.zip")));

    SAME(refusal_of(loop, crc.extracted({Pick{Path{}, desk.at("crc")}})), std::string{"malformed: A zip member is damaged (its CRC-32 differs): a/two.txt."});
    SAME(desk.got("crc/a/one.txt"), std::string{"none"});
    OUGHT(!std::filesystem::exists(std::filesystem::path{desk.at("crc").text()}));
    SAME(refusal_of(loop, tal::wait(loop, tal::archive::opened(desk.at("short.zip"))).extracted({Pick{Path{}, desk.at("short")}})),
         std::string{"malformed: A zip member is cut short: a/one.txt."});
    SAME(refusal_of(loop, tal::wait(loop, tal::archive::opened(desk.at("long.zip"))).extracted({Pick{Path{}, desk.at("long")}})),
         std::string{"malformed: A zip member is longer than the archive says: a/one.txt."});

    desk.put("open.zip", zipped({Entry{"open.txt", "good"}.packed_by(8).packed_as(Bytes{std::byte{0}, std::byte{4}, std::byte{0}, std::byte{0xFB}, std::byte{0xFF}, std::byte{'g'},
                                                                                std::byte{'o'}, std::byte{'o'}, std::byte{'d'}})}));
    SAME(refusal_of(loop, tal::wait(loop, tal::archive::opened(desk.at("open.zip"))).extracted({Pick{Path{}, desk.at("open")}})),
         std::string{"malformed: A zip member is cut short: open.txt."});

    Bytes headless = zipped({Entry{"a.txt", "a"}});

    headless[0] = std::byte{0};
    desk.put("local.zip", headless);
    SAME(refusal_of(loop, tal::wait(loop, tal::archive::opened(desk.at("local.zip"))).extracted({Pick{Path{}, desk.at("local")}})),
         std::string{"malformed: A zip local header is damaged."});

    SAME(kind_of(loop, crc.extracted({Pick{Path{"a/one.txt"}, desk.at("one.txt")}, Pick{Path{"a/two.txt"}, desk.at("two.txt")}})), named(Kind::malformed));
    SAME(desk.got("one.txt"), std::string{"none"});

    std::filesystem::create_directories(std::filesystem::path{desk.at("kept").text()});
    SAME(kind_of(loop, crc.extracted({Pick{Path{}, desk.at("kept")}}, tal::fs::Conflict::replace)), named(Kind::malformed));
    OUGHT(std::filesystem::exists(std::filesystem::path{desk.at("kept").text()}));
    SAME(desk.listed("kept").size(), std::size_t{1});
    SAME(desk.listed("kept/a"), std::vector<std::string>{"one.txt"});

    std::string all;

    for (const std::string& name : desk.listed()) all += name + " ";

    SAME(all, std::string{"a.zip crc.zip f kept local.zip long.zip open.zip out short.zip single.txt "});
}

TRIAL("Zip: conflicts settled as a copy settles them, for files and for folders") {
    tal::Loop loop;
    const Desk desk;

    desk.put("a.zip", zipped({Entry{"a.txt", "new"}, Entry{"d/b.txt", "new b"}, Entry{"dd/c.txt", "c"}}));

    const Archive archive = tal::wait(loop, tal::archive::opened(desk.at("a.zip")));

    desk.put("a.txt", tal::bytes("old"));
    SAME(kind_of(loop, archive.extracted({Pick{Path{"a.txt"}, desk.at("a.txt")}})), named(Kind::exists));
    SAME(desk.got("a.txt"), std::string{"old"});

    tal::Progress progress;

    (void)tal::wait(loop, archive.extracted({Pick{Path{"a.txt"}, desk.at("a.txt")}}, tal::fs::Conflict::skip, &progress));
    SAME(desk.got("a.txt"), std::string{"old"});
    SAME(progress.done(), std::uint64_t{3});

    (void)tal::wait(loop, archive.extracted({Pick{Path{"a.txt"}, desk.at("a.txt")}}, tal::fs::Conflict::rename));
    SAME(desk.got("a (2).txt"), std::string{"new"});
    (void)tal::wait(loop, archive.extracted({Pick{Path{"a.txt"}, desk.at("a.txt")}}, tal::fs::Conflict::rename));
    SAME(desk.got("a (3).txt"), std::string{"new"});
    (void)tal::wait(loop, archive.extracted({Pick{Path{"a.txt"}, desk.at("a.txt")}}, tal::fs::Conflict::replace));
    SAME(desk.got("a.txt"), std::string{"new"});

    std::filesystem::create_directories(std::filesystem::path{desk.at("d").text()});
    desk.put("d/old.txt", tal::bytes("kept"));

    SAME(kind_of(loop, archive.extracted({Pick{Path{"d"}, desk.at("d")}})), named(Kind::exists));
    (void)tal::wait(loop, archive.extracted({Pick{Path{"d"}, desk.at("d")}}, tal::fs::Conflict::skip));
    SAME(desk.listed("d"), std::vector<std::string>{"old.txt"});
    (void)tal::wait(loop, archive.extracted({Pick{Path{"d"}, desk.at("d")}}, tal::fs::Conflict::rename));
    SAME(desk.got("d (2)/b.txt"), std::string{"new b"});
    SAME(desk.listed("d (2)"), std::vector<std::string>{"b.txt"});
    (void)tal::wait(loop, archive.extracted({Pick{Path{"d"}, desk.at("d")}}, tal::fs::Conflict::replace));
    SAME(desk.listed("d"), (std::vector<std::string>{"b.txt", "old.txt"}));

    desk.put("file", tal::bytes("x"));
    SAME(kind_of(loop, archive.extracted({Pick{Path{"d"}, desk.at("file")}}, tal::fs::Conflict::replace)), named(Kind::exists));
    SAME(desk.got("file"), std::string{"x"});
    SAME(kind_of(loop, Archive{}.extracted({})), named(Kind::other));
}

TRIAL("Tar: ustar, GNU long names, pax, links and devices, base-256 and signed checksums; plain and through gzip") {
    tal::Loop loop;
    const Desk desk;
    const std::string long_name = std::string(150, 'n') + ".txt";
    const std::vector<Record> records{Record{"films/", '5', ""}.moded(0750),
                                      Record{"kino.txt", '0', "Poirot"}.moded(0640).timed(1600000000).prefixed("films"),
                                      Record{"././@LongLink", 'L', long_name + std::string(1, '\0')},
                                      Record{"cut", '0', "long"},
                                      pax({"path=pax/\xC3\xB3.txt", "mtime=1500000000.5", "size=3", "comment=x"}),
                                      {"ignored", '0', "pax"},
                                      {"latin\xE9.txt", '0', "l"},
                                      Record{"link", '2', ""}.moded(0777),
                                      Record{"hard", '1', ""}.sized(100),
                                      {"dev", '3', ""},
                                      {"fifo", '6', "fifodata"},
                                      {"global", 'g', "10 a=b\n"},
                                      Record{"big.txt", '0', "base"}.sized(4, true),
                                      Record{"signed.txt", '0', "s"}.signed_up(),
                                      {"../up", '0', "u"},
                                      {"plain", '\0', "old"},
                                      {"contiguous", '7', "c"}};

    desk.put("a.tar", tarred(records));
    desk.put("a.tar.gz", gzipped(tarred(records)));

    for (const std::string name : {"a.tar", "a.tar.gz"}) {
        const Archive archive = tal::wait(loop, tal::archive::opened(desk.at(name)));

        SAME(shown(archive), std::string{"films/ films/kino.txt " + long_name + " pax/\xC3\xB3.txt latin\xC3\xA9.txt link!link hard!link big.txt signed.txt up!unsafe plain contiguous pax/"});
        SAME(member(archive, "films/kino.txt").rights.value_or(0), std::uint16_t{0640});
        SAME(member(archive, "films").rights.value_or(0), std::uint16_t{0750});
        SAME(std::chrono::duration_cast<std::chrono::seconds>(member(archive, "films/kino.txt").modified.time_since_epoch()).count(), std::int64_t{1600000000});
        SAME(std::chrono::duration_cast<std::chrono::seconds>(member(archive, "pax/\xC3\xB3.txt").modified.time_since_epoch()).count(), std::int64_t{1500000000});
        SAME(member(archive, "pax/\xC3\xB3.txt").size, std::uint64_t{3});
        SAME(member(archive, "hard").size, std::uint64_t{0});
        SAME(member(archive, "big.txt").size, std::uint64_t{4});

        tal::Progress progress;
        const std::vector<Member> refused = tal::wait(loop, archive.extracted({Pick{Path{}, desk.at(name + ".out")}}, tal::fs::Conflict::fail, &progress));

        SAME(refused.size(), std::size_t{3});
        SAME(desk.got(name + ".out/films/kino.txt"), std::string{"Poirot"});
        SAME(desk.got(name + ".out/" + long_name), std::string{"long"});
        SAME(desk.got(name + ".out/pax/\xC3\xB3.txt"), std::string{"pax"});
        SAME(desk.got(name + ".out/latin\xC3\xA9.txt"), std::string{"l"});
        SAME(desk.got(name + ".out/big.txt"), std::string{"base"});
        SAME(desk.got(name + ".out/signed.txt"), std::string{"s"});
        SAME(desk.got(name + ".out/plain"), std::string{"old"});
        SAME(desk.got(name + ".out/contiguous"), std::string{"c"});
        SAME(progress.done(), progress.total());

        (void)tal::wait(loop, archive.extracted({Pick{Path{"contiguous"}, desk.at(name + ".c")}, Pick{Path{"films/kino.txt"}, desk.at(name + ".k")}}));
        SAME(desk.got(name + ".c"), std::string{"c"});
        SAME(desk.got(name + ".k"), std::string{"Poirot"});
    }
}

TRIAL("Tar: what is not a tar, a damaged header, a stream cut short and extensions too long are refused") {
    tal::Loop loop;
    const Desk desk;

    desk.put("empty.tar", Bytes{});
    desk.put("zeros.tar", Bytes(1024));
    desk.put("text.tar", tal::bytes(std::string(512, 'x')));
    desk.put("damaged.tar", tarred({Record{"a", '0', "a"}, Record{"b", '0', "b"}.miscounted()}));
    const Bytes whole = tarred({Record{"a", '0', "abc"}}, false);

    desk.put("cut.tar", Bytes(whole.begin(), whole.begin() + 600));
    desk.put("half.tar", Bytes(300));
    desk.put("long.tar", tarred({Record{"././@LongLink", 'L', ""}.sized(std::uint64_t{1} << 21)}));
    desk.put("huge.tar", tarred({Record{"a", '0', ""}.sized(UINT64_MAX - 100, true)}));
    desk.put("ended.tar", tarred({Record{"a", '0', "abc"}}, false));

    SAME(refusal_of(loop, tal::archive::opened(desk.at("empty.tar"))), std::string{"malformed: Not a tar archive."});
    SAME(shown(tal::wait(loop, tal::archive::opened(desk.at("zeros.tar")))), std::string{});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("text.tar"))), std::string{"malformed: Not a tar archive."});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("damaged.tar"))), std::string{"malformed: A tar header is damaged."});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("cut.tar"))), std::string{"malformed: The tar archive is cut short."});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("half.tar"))), std::string{"malformed: The tar archive is cut short."});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("long.tar"))), std::string{"malformed: A tar extension header is too long."});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("huge.tar"))), std::string{"malformed: A tar header is damaged."});
    SAME(shown(tal::wait(loop, tal::archive::opened(desk.at("ended.tar")))), std::string{"a"});

    Bytes gz = gzipped(tarred({Record{"a", '0', "abc"}}));

    gz.resize(700);
    desk.put("cut.tar.gz", gz);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("cut.tar.gz"))), std::string{"malformed: The gzip stream is cut short."});

    desk.put("bad.tgz", tal::bytes("not gzip at all"));
    SAME(refusal_of(loop, tal::archive::opened(desk.at("bad.tgz"))), std::string{"malformed: The compressed data is damaged: not gzip."});

    std::vector<Record> many;

    for (int one = 0; one < 4; ++one) many.push_back(Record{"m" + std::to_string(one), '0', ""});

    desk.put("many.tar", tarred(many));
    SAME(tal::wait(loop, tal::archive::opened(desk.at("many.tar"))).members().size(), std::size_t{4});
}

TRIAL("Zip: the finer points: hosts, attributes, extra fields that do not apply, odd DOS dates, the end record and its bounds") {
    tal::Loop loop;
    const Desk desk;
    Bytes needless;

    put(needless, 0x0001, 2);
    put(needless, 8, 2);
    put(needless, 99, 8);

    Bytes unstamped;

    put(unstamped, 0x5455, 2);
    put(unstamped, 5, 2);
    put(unstamped, 0, 1);
    put(unstamped, 1600000000, 4);

    desk.put("fine.zip", zipped({Entry{"./", ""},
                                 Entry{"../up/x.txt", "u"},
                                 Entry{"dosrights.txt", "r"}.by(0x0014).attributes(0100600U << 16),
                                 Entry{"dosdir", ""}.by(0x0014).attributes(0x10),
                                 Entry{"folder/", "xx"},
                                 Entry{"plain64.txt", "pq"}.with(needless),
                                 Entry{"nostamp.txt", "n"}.with(unstamped),
                                 Entry{"month.txt", "m"}.dated(0, static_cast<std::uint16_t>((45U << 9) | (13U << 5) | 1U))},
                                "", {}, false, std::string{"PK\x05\x06", 4} + std::string(16, '\0') + "\xFF\xFF"));

    const Archive fine = tal::wait(loop, tal::archive::opened(desk.at("fine.zip")));

    SAME(shown(fine), std::string{"up/x.txt!unsafe dosrights.txt dosdir/ folder/ plain64.txt nostamp.txt month.txt"});
    OUGHT(!member(fine, "dosrights.txt").rights);
    SAME(member(fine, "folder").size, std::uint64_t{0});
    SAME(member(fine, "plain64.txt").size, std::uint64_t{2});
    OUGHT(std::chrono::duration_cast<std::chrono::seconds>(member(fine, "nostamp.txt").modified.time_since_epoch()).count() != 1600000000);
    SAME(std::chrono::duration_cast<std::chrono::seconds>(member(fine, "month.txt").modified.time_since_epoch()).count(), std::int64_t{315532800});

    desk.put("offset.zip", zipped({Entry{"a.txt", "a"}}, "", {}, true, "comment", true));
    SAME(shown(tal::wait(loop, tal::archive::opened(desk.at("offset.zip")))), std::string{"a.txt"});

    const std::array<std::byte, 4> central{std::byte{0x50}, std::byte{0x4B}, std::byte{1}, std::byte{2}};
    Bytes near = zipped({Entry{"a.txt", "a"}, Entry{"b.txt", "b"}});
    const auto first = std::search(near.begin(), near.end(), central.begin(), central.end());
    const auto second = static_cast<std::size_t>(std::search(first + 1, near.end(), central.begin(), central.end()) - near.begin());

    near[second + 42] = std::byte{30};
    desk.put("near.zip", near);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("near.zip"))), std::string{"malformed: Members of the zip archive overlap, as in a zip bomb."});

    near[second + 42] = std::byte{31};

    for (std::size_t at = 1; at < 4; ++at) near[second + 42 + at] = std::byte{0};

    desk.put("touching.zip", near);
    SAME(shown(tal::wait(loop, tal::archive::opened(desk.at("touching.zip")))), std::string{"a.txt b.txt"});

    Bytes overrun = zipped({Entry{"a.txt", "a"}});
    const auto entry = static_cast<std::size_t>(std::search(overrun.begin(), overrun.end(), central.begin(), central.end()) - overrun.begin());

    overrun[entry + 28] = std::byte{0xFF};
    overrun[entry + 29] = std::byte{0xFF};
    desk.put("overrun.zip", overrun);
    SAME(refusal_of(loop, tal::archive::opened(desk.at("overrun.zip"))), std::string{"malformed: The central directory is damaged."});
}

TRIAL("Tar: sizes that are no numbers, a lone block of zeros, links that say a size, pax's size over the header's") {
    tal::Loop loop;
    const Desk desk;

    desk.put("negative.tar", tarred({Record{"a", '0', ""}.size_field(std::string{"\xFF"} + std::string(11, '\x01'))}));
    desk.put("letters.tar", tarred({Record{"a", '0', ""}.size_field(std::string{"0000000012x\0", 12})}));
    desk.put("wide.tar", tarred({Record{"a", '0', ""}.size_field(std::string{"\x80"} + std::string(11, '\xFF'))}));

    SAME(refusal_of(loop, tal::archive::opened(desk.at("negative.tar"))), std::string{"malformed: A tar header is damaged."});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("letters.tar"))), std::string{"malformed: A tar header is damaged."});
    SAME(refusal_of(loop, tal::archive::opened(desk.at("wide.tar"))), std::string{"malformed: A tar header is damaged."});

    desk.put("binary.tar", tarred({Record{"a", '0', "a"}.overwritten(100, std::string{"\xFF\0\0\0\0\0\0\x01", 8}).overwritten(136, std::string{"\x80"} + std::string(11, '\xFF'))}));

    const Archive binary = tal::wait(loop, tal::archive::opened(desk.at("binary.tar")));

    OUGHT(!binary.members()[0].rights);
    SAME(binary.members()[0].modified.time_since_epoch().count(), std::chrono::system_clock::duration::rep{0});

    desk.put("gap.tar", tarred({Record{"a", '0', "a"}, Record{"", 'Z', ""}, Record{"b", '0', "b"}}));
    SAME(shown(tal::wait(loop, tal::archive::opened(desk.at("gap.tar")))), std::string{"a b"});

    desk.put("sized.tar", tarred({Record{"hard", '1', ""}.sized(100), Record{"folder/", '5', ""}.sized(600), Record{"after", '0', "after"},
                                  pax({"size=5"}), Record{"paxed", '0', "paxed"}.sized(0)}));

    const Archive sized = tal::wait(loop, tal::archive::opened(desk.at("sized.tar")));

    SAME(shown(sized), std::string{"hard!link folder/ after paxed"});
    (void)tal::wait(loop, sized.extracted({Pick{Path{"paxed"}, desk.at("paxed.txt")}, Pick{Path{"after"}, desk.at("after.txt")}}));
    SAME(desk.got("paxed.txt"), std::string{"paxed"});
    SAME(desk.got("after.txt"), std::string{"after"});
}
