#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/bytes.hpp"

namespace tal::testing {

[[nodiscard]] Bytes lump(std::string_view hex);

[[nodiscard]] std::string zlibbed(std::string_view given);

[[nodiscard]] std::string fielded(const std::vector<std::array<std::uint64_t, 3>>& entries, const std::array<int, 3>& widths);

[[nodiscard]] std::string png(std::string_view text, std::size_t columns, const std::vector<int>& filters);

struct Packing {
    std::string data;
    std::size_t first = 0;
};

[[nodiscard]] Packing packing(const std::vector<std::pair<std::uint32_t, std::string>>& objects, std::string_view lead = {});

class Assembly {
public:
    explicit Assembly(std::string_view version = "1.4", std::string_view junk = {});

    void object(std::uint32_t number, std::string_view body);

    void stream(std::uint32_t number, std::string_view dict, std::string_view data);

    void packed(std::uint32_t number, const std::vector<std::pair<std::uint32_t, std::string>>& objects,
                const std::function<std::string(std::string_view)>& encode = zlibbed, std::string_view filter = " /Filter /FlateDecode",
                std::string_view lead = {});

    void inside(std::uint32_t number, std::uint32_t pack, std::uint32_t index);

    [[nodiscard]] std::vector<std::array<std::uint64_t, 3>> entries(std::uint32_t number);

    std::size_t xref_given(std::uint32_t number, std::string_view dict, std::string_view data);

    std::size_t table(std::string_view trailer);

    std::size_t xref_stream(std::uint32_t number, std::string_view trailer, bool predicted);

    [[nodiscard]] std::size_t offset(std::uint32_t number) const { return offsets_.at(number); }

    [[nodiscard]] std::string& text() { return text_; }

    [[nodiscard]] Bytes bytes() const;

private:
    [[nodiscard]] static std::string padded(std::size_t value, std::size_t width);

    [[nodiscard]] std::string sized() const;

    void closed(std::size_t at, const std::string& trailer);

    std::string text_;
    std::size_t base_ = 0;
    std::size_t previous_ = 0;
    std::map<std::uint32_t, std::size_t> offsets_;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> inside_;
    std::vector<std::uint32_t> fresh_;
};

}
