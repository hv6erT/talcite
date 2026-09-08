#pragma once

#include <stdexcept>
#include <string>

#include "talcite/address.hpp"

namespace talcite::net {

class Trouble : public std::runtime_error {
public:
    explicit Trouble(const std::string& said, int code = 0, Address where = {});

    [[nodiscard]] int code() const { return code_; }

    [[nodiscard]] const Address& where() const { return where_; }

private:
    Address where_;
    int code_ = 0;
};

[[nodiscard]] std::string spoken(int code);

}
