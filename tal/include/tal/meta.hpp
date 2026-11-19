#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/fs.hpp"
#include "tal/path.hpp"
#include "tal/port.hpp"
#include "tal/task.hpp"

namespace tal::meta {

enum class Scheme : std::uint8_t { id3v2, id3v1, vorbis, ape, itunes, pdf, matroska, exif, xmp, iptc, riff };

using Value = std::variant<std::string, std::int64_t>;

struct Field {
    Scheme scheme = Scheme::id3v2;
    std::string key;
    Value value;

    friend bool operator==(const Field&, const Field&) = default;
};

enum class Role : std::uint8_t { front, back, artist, other };

struct Picture {
    Role role = Role::other;
    std::string type;
    std::string said;
    std::size_t size = 0;

    friend bool operator==(const Picture&, const Picture&) = default;
};

struct Properties {
    std::optional<std::chrono::milliseconds> duration;
    std::optional<std::uint32_t> rate;
    std::optional<std::uint16_t> channels;
    std::optional<std::uint16_t> depth;
    std::optional<std::uint32_t> bitrate;

    friend bool operator==(const Properties&, const Properties&) = default;
};

enum class Transfer : std::uint8_t { standard, pq, hlg };

struct Stream {
    enum class Kind : std::uint8_t { video, audio, subtitle };

    Kind kind = Kind::video;
    std::string codec;
    std::string language;
    std::string name;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<std::uint16_t> depth;
    std::optional<Transfer> transfer;
    std::optional<std::uint16_t> channels;
    std::optional<std::uint32_t> rate;
    std::optional<std::uint32_t> bitrate;

    friend bool operator==(const Stream&, const Stream&) = default;
};

struct Sheet {
    double width = 0;
    double height = 0;
    std::uint32_t pages = 0;

    friend bool operator==(const Sheet&, const Sheet&) = default;
};

struct Document {
    std::string version;
    std::uint32_t pages = 0;
    std::vector<Sheet> sheets;
    std::uint32_t with_text = 0;
    std::uint32_t fonts = 0;
    std::uint32_t embedded = 0;
    std::uint32_t bookmarks = 0;
    std::uint32_t links = 0;
    bool form = false;
    bool encrypted = false;

    friend bool operator==(const Document&, const Document&) = default;
};

struct Place {
    double latitude = 0;
    double longitude = 0;
    std::optional<double> altitude;

    friend bool operator==(const Place&, const Place&) = default;
};

struct Photo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t orientation = 1;
    std::string make;
    std::string model;
    std::string taken;
    std::optional<Place> place;

    friend bool operator==(const Photo&, const Photo&) = default;
};

struct Description {
    std::string type;
    Properties properties;
    std::vector<Stream> streams;
    std::vector<Field> fields;
    std::vector<Picture> pictures;
    std::optional<Document> document;
    std::optional<Photo> photo;
    bool damaged = false;

    friend bool operator==(const Description&, const Description&) = default;
};

class Reader {
public:
    virtual ~Reader() = default;

    [[nodiscard]] virtual Task<std::uint64_t> size() = 0;

    [[nodiscard]] virtual Task<Bytes> read(std::uint64_t at, std::size_t much) = 0;
};

class Held final : public Reader {
public:
    explicit Held(Bytes given) : bytes_(std::move(given)) {}

    [[nodiscard]] Task<std::uint64_t> size() override;

    [[nodiscard]] Task<Bytes> read(std::uint64_t at, std::size_t much) override;

private:
    Bytes bytes_;
};

[[nodiscard]] Task<Description> described(Reader& from);

[[nodiscard]] Task<Description> described(Path where, Port<fs::Files> files = {});

[[nodiscard]] Task<Bytes> pictured(Reader& from, std::size_t which);

[[nodiscard]] Task<Bytes> pictured(Path where, std::size_t which, Port<fs::Files> files = {});

}
