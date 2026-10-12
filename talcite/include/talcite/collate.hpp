#pragma once

#include <string>
#include <string_view>

namespace talcite {

class Collation {
public:
    explicit Collation(std::string_view language = {});

    [[nodiscard]] std::string key(std::string_view text) const;

    [[nodiscard]] int compare(std::string_view one, std::string_view two) const;

    [[nodiscard]] const std::string& language() const { return language_; }

private:
    std::string language_;
    int tailored_ = -1;
};

}
