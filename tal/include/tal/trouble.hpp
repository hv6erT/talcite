#pragma once

#include <exception>
#include <string>
#include <string_view>

namespace tal {

enum class Kind { missing, denied, exists, busy, full, refused, broken, malformed, unsupported, overdue, other };

class Trouble : public std::exception {
public:
    Trouble(Kind kind, std::string said, std::string where = {}, int code = 0);

    [[nodiscard]] Kind kind() const { return kind_; }

    [[nodiscard]] int code() const { return code_; }

    [[nodiscard]] const std::string& where() const { return where_; }

    [[nodiscard]] const char* what() const noexcept override { return said_.c_str(); }

private:
    Kind kind_;
    std::string said_;
    std::string where_;
    int code_;
};

class Overdue : public Trouble {
public:
    Overdue();
};

class Cancelled : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override;
};

[[nodiscard]] std::string_view spoken(Kind kind);

}
