#pragma once

#include <stdexcept>
#include <string>

namespace talcite {

class Trouble : public std::runtime_error {
public:
    explicit Trouble(const std::string& said, int code = 0);

    [[nodiscard]] int code() const { return code_; }

private:
    int code_ = 0;
};

}
