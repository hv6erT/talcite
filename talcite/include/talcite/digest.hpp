#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "talcite/bytes.hpp"

namespace talcite {

enum class Sum : std::uint8_t { md5, sha1, sha256, sha512 };

class Digesting {
public:
    virtual ~Digesting() = default;

    Digesting(const Digesting&) = delete;
    Digesting& operator=(const Digesting&) = delete;
    Digesting(Digesting&&) = delete;
    Digesting& operator=(Digesting&&) = delete;

    virtual void feed(const Bytes& what) = 0;

    [[nodiscard]] virtual Bytes done() = 0;

protected:
    Digesting() = default;
};

class DigestProvider {
public:
    virtual ~DigestProvider() = default;

    DigestProvider(const DigestProvider&) = delete;
    DigestProvider& operator=(const DigestProvider&) = delete;
    DigestProvider(DigestProvider&&) = delete;
    DigestProvider& operator=(DigestProvider&&) = delete;

    [[nodiscard]] virtual std::string_view name() const = 0;

    [[nodiscard]] virtual std::unique_ptr<Digesting> started(Sum which, const Bytes* key) = 0;

protected:
    DigestProvider() = default;
};

void spared(std::shared_ptr<DigestProvider> spare);

[[nodiscard]] bool summing();

[[nodiscard]] Bytes summed(Sum which, const Bytes& what);

[[nodiscard]] Bytes stamped(Sum which, const Bytes& key, const Bytes& what);

[[nodiscard]] Bytes derived(Sum which, const Bytes& salt, const Bytes& secret, const Bytes& info, std::size_t much);

class Digest {
public:
    struct Held;

    explicit Digest(Sum which);

    Digest(Sum which, const Bytes& key);

    ~Digest();

    Digest(Digest&& other) noexcept;
    Digest& operator=(Digest&& other) noexcept;

    Digest(const Digest&) = delete;
    Digest& operator=(const Digest&) = delete;

    void feed(const Bytes& what);

    [[nodiscard]] Bytes done();

private:
    std::unique_ptr<Held> held_;
};

}
