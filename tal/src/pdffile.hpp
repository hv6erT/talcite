#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/meta.hpp"
#include "tal/task.hpp"

#include "pdfing.hpp"
#include "tagging.hpp"

namespace tal::meta::tagging::pdf {

struct Place {
    enum class Kind : std::uint8_t { free, direct, packed };

    Kind kind = Kind::free;
    std::uint64_t at = 0;
    std::uint32_t index = 0;
};

struct Slot {
    std::uint32_t number = 0;
    std::uint32_t at = 0;
    std::uint32_t end = 0;
};

struct Packed {
    Bytes data;
    std::vector<Slot> table;
    std::vector<std::uint32_t> numbers;
    std::size_t first = 0;
};

struct Section {
    Dict trailer;
    std::vector<std::pair<std::uint32_t, Place>> entries;
};

struct Scan {
    std::set<std::uint64_t> trailers;
    std::set<std::uint32_t> streams;
    std::set<std::uint32_t> packs;
    std::set<std::uint32_t> catalogs;
};

struct Loaded {
    std::optional<Object> object;
    bool missing = false;
};

using Box = std::array<double, 4>;

struct Inherited {
    const Object* resources = nullptr;
    Box media{0, 0, 612, 792};
    std::optional<Box> crop;
    std::int64_t rotate = 0;
};

struct Leaf {
    const Dict* page = nullptr;
    Inherited from;
};

class File {
public:
    File(Reader& from, std::uint64_t size, Gathered& out);

    ~File();

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    [[nodiscard]] Task<void> read();

    [[nodiscard]] Task<const Dict*> opened();

    [[nodiscard]] Task<std::vector<Leaf>> leaves(const Dict& catalog);

    [[nodiscard]] Task<const Object*> numbered(std::uint32_t number);

    [[nodiscard]] Task<const Object*> resolved(const Object* one);

    [[nodiscard]] Task<std::optional<Bytes>> decoded(const Stream& one);

    [[nodiscard]] bool sealed() const { return trailer_.find("Encrypt") != nullptr; }

private:
    [[nodiscard]] Task<Bytes> fetched(std::uint64_t at, std::size_t much);

    [[nodiscard]] std::optional<std::uint64_t> shifted(std::uint64_t at) const;

    [[nodiscard]] Task<void> headed();

    [[nodiscard]] Task<std::optional<std::uint64_t>> started();

    [[nodiscard]] Task<bool> chained(std::uint64_t first);

    [[nodiscard]] Task<std::optional<Section>> section(std::uint64_t at);

    [[nodiscard]] Task<std::optional<Section>> tabled(std::uint64_t at);

    [[nodiscard]] Task<std::optional<Section>> streamed(std::uint64_t at);

    void placed(const std::vector<std::pair<std::uint32_t, Place>>& entries);

    void merged(const Dict& older);

    [[nodiscard]] Task<void> rebuild();

    [[nodiscard]] Task<bool> catalogued(std::uint32_t number);

    void scanned(const Bytes& text, std::uint64_t at, std::size_t to, Scan& into);

    [[nodiscard]] Task<std::optional<Object>> indirect(std::uint64_t at, std::optional<std::uint32_t> number);

    [[nodiscard]] Task<std::optional<Object>> located(std::uint64_t at, std::uint32_t number);

    [[nodiscard]] Task<Loaded> loaded(std::uint32_t number);

    [[nodiscard]] Task<const Packed*> unpacked(std::uint32_t number);

    [[nodiscard]] Task<std::optional<Bytes>> data_of(const Stream& one);

    [[nodiscard]] Task<std::optional<Bytes>> ended(std::uint64_t at);

    [[nodiscard]] std::optional<Bytes> inflated(const Bytes& packed);

    [[nodiscard]] std::optional<Bytes> charged(std::optional<Bytes> data);

    [[nodiscard]] Task<std::vector<Leaf>> paged(const Object& root);

    [[nodiscard]] Task<void> page(const Dict& leaf, const Inherited& from);

    void sheet(double width, double height);

    [[nodiscard]] Task<bool> lettered(const Object* resources, std::size_t depth);

    [[nodiscard]] Task<bool> drew(const Dict* objects, std::size_t depth);

    [[nodiscard]] Task<bool> formed(const Object& value, std::size_t depth);

    [[nodiscard]] Task<void> font(const Object& value);

    [[nodiscard]] Task<bool> embedded(const Dict* font);

    [[nodiscard]] Task<void> bookmarked(const Dict& catalog);

    [[nodiscard]] Task<void> informed();

    Reader& from_;
    std::uint64_t size_;
    Gathered& out_;
    std::uint64_t budget_;
    std::uint64_t fetched_ = 0;
    Document document_;
    std::uint64_t shift_ = 0;
    std::string version_;
    Dict trailer_;
    std::map<std::uint32_t, Place> places_;
    std::map<std::uint32_t, Object> cache_;
    std::deque<Packed> packs_;
    std::map<std::uint32_t, const Packed*> packed_;
    std::set<std::uint32_t> resolving_;
    std::map<const Dict*, bool> drawn_;
    std::map<std::uint32_t, bool> forms_;
    std::set<const Dict*> named_;
    std::set<std::uint32_t> fonts_;
    std::set<const Array*> annotated_;
    std::size_t spent_ = 0;
    std::uint64_t looks_ = 0;
    bool rebuilt_ = false;
    bool settled_ = false;
    const Object none_{Null{}};
};

}
