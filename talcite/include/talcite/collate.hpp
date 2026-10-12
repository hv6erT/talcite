#pragma once

#include <string>
#include <string_view>

namespace talcite {

struct CollationShape {
    bool numeric = true;
    bool exact = false;
};

class Collation {
public:
    explicit Collation(std::string_view language = {}, CollationShape shape = {});

    [[nodiscard]] std::string key(std::string_view text) const;

    [[nodiscard]] int compare(std::string_view one, std::string_view two) const;

    [[nodiscard]] const std::string& language() const { return language_; }

    [[nodiscard]] const CollationShape& shape() const { return shape_; }

private:
    std::string language_;
    CollationShape shape_;
    int tailored_ = -1;
};

}
