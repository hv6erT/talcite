#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/fs.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"

namespace talcite::meta {

enum class Scheme : std::uint8_t { id3v2, id3v1, vorbis, ape, itunes, pdf };

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

struct Description {
    std::string type;
    Properties properties;
    std::vector<Stream> streams;
    std::vector<Field> fields;
    std::vector<Picture> pictures;
    std::optional<Document> document;
    bool damaged = false;

    friend bool operator==(const Description&, const Description&) = default;
};

class Reader {
public:
    virtual ~Reader() = default;

    [[nodiscard]] virtual std::uintmax_t size() const = 0;

    [[nodiscard]] virtual Bytes read(std::uintmax_t at, std::size_t much) = 0;
};

}

namespace talcite {

class MetaProvider : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "Meta"; }

    [[nodiscard]] virtual std::string describe(meta::Reader& from, meta::Description& into) = 0;

    [[nodiscard]] virtual std::string picture(meta::Reader& from, std::size_t which, Bytes& into) = 0;
};

template <>
class Seam<MetaProvider> {
public:
    Seam() = default;

    explicit Seam(std::shared_ptr<MetaProvider> given) : provider_(std::move(given)) {}

    [[nodiscard]] bool bound() const { return provider_ != nullptr; }

    [[nodiscard]] MetaProvider& provider() const;

    [[nodiscard]] Promise<meta::Description> described(fs::path where) const;

    [[nodiscard]] Promise<Bytes> pictured(fs::path where, std::size_t which) const;

private:
    std::shared_ptr<MetaProvider> provider_;
};

}
