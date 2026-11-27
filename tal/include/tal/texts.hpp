#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/fs.hpp"
#include "tal/path.hpp"
#include "tal/task.hpp"

namespace tal {

struct Spoken {
    std::string code;
    std::string name;

    friend bool operator==(const Spoken&, const Spoken&) = default;
};

class Texts {
public:
    Texts(std::span<const std::string_view> keys, std::string_view english);

    [[nodiscard]] Task<void> read(std::vector<Path> folders, std::string code, std::shared_ptr<fs::Files> files = nullptr);

    [[nodiscard]] const std::string& code() const { return code_; }

    [[nodiscard]] const std::string& said(std::size_t key) const;

    [[nodiscard]] const std::string& counted(std::size_t key, std::uint64_t many) const;

    [[nodiscard]] char32_t letter(std::size_t key) const;

    [[nodiscard]] Task<std::vector<Spoken>> languages(std::vector<Path> folders, std::shared_ptr<fs::Files> files = nullptr) const;

    [[nodiscard]] static std::string nearest(std::string_view wanted, const std::vector<std::string>& known);

private:
    struct Said {
        std::array<std::string, 6> forms;
        bool counted = false;
        bool letter = false;
        bool english = true;
    };

    std::vector<std::string> keys_;
    std::string name_;
    std::vector<Said> base_;
    std::vector<Said> said_;
    std::string code_;
};

[[nodiscard]] std::string filled(std::string_view said, std::initializer_list<std::string_view> with);

}
