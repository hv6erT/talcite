#pragma once

#include <memory>
#include <string>

#include "talcite/bytes.hpp"

namespace talcite::net {

class Secure {
public:
    virtual ~Secure() = default;

    Secure(const Secure&) = delete;
    Secure& operator=(const Secure&) = delete;
    Secure(Secure&&) = delete;
    Secure& operator=(Secure&&) = delete;

    [[nodiscard]] virtual bool ready() const = 0;

    virtual void push(Bytes given) = 0;

    [[nodiscard]] virtual Bytes pull() = 0;

    virtual void write(Bytes given) = 0;

    [[nodiscard]] virtual Bytes read() = 0;

    virtual void close() = 0;

    [[nodiscard]] virtual std::string amiss() const = 0;

protected:
    Secure() = default;
};

class Keeper {
public:
    virtual ~Keeper() = default;

    Keeper(const Keeper&) = delete;
    Keeper& operator=(const Keeper&) = delete;
    Keeper(Keeper&&) = delete;
    Keeper& operator=(Keeper&&) = delete;

    [[nodiscard]] virtual std::unique_ptr<Secure> client(std::string host) = 0;

    [[nodiscard]] virtual std::unique_ptr<Secure> server() = 0;

protected:
    Keeper() = default;
};

class Guard {
public:
    Guard() = default;

    explicit Guard(std::shared_ptr<Keeper> how);

    [[nodiscard]] bool held() const { return how_ != nullptr; }

    [[nodiscard]] std::unique_ptr<Secure> client(std::string host) const;

    [[nodiscard]] std::unique_ptr<Secure> server() const;

private:
    std::shared_ptr<Keeper> how_;
};

}
