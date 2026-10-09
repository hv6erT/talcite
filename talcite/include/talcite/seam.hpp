#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "talcite/bytes.hpp"
#include "talcite/trouble.hpp"

namespace talcite {

class Provider {
public:
    virtual ~Provider() = default;

    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    Provider(Provider&&) = delete;
    Provider& operator=(Provider&&) = delete;

    [[nodiscard]] virtual std::string_view name() const = 0;

protected:
    Provider() = default;
};

class Unbound : public Trouble {
public:
    explicit Unbound(std::string_view kind);
};

class Refused : public Trouble {
public:
    Refused(std::string_view kind, std::string_view said);
};

template <typename Made>
class Codec : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return Made::kind(); }

    [[nodiscard]] virtual std::string decode(const Bytes& given, Made& into) = 0;

    [[nodiscard]] virtual std::string encode(const Made& given, Bytes& into) = 0;
};

template <typename Wanted>
class Seam {
public:
    Seam() = default;

    explicit Seam(std::shared_ptr<Wanted> given) : provider_(std::move(given)) {}

    [[nodiscard]] bool bound() const { return provider_ != nullptr; }

    [[nodiscard]] Wanted& provider() const {
        if (provider_ == nullptr) throw Unbound{Wanted::kind()};

        return *provider_;
    }

private:
    std::shared_ptr<Wanted> provider_;
};

template <typename Made>
class Seam<Codec<Made>> {
public:
    Seam() = default;

    explicit Seam(std::shared_ptr<Codec<Made>> given) : provider_(std::move(given)) {}

    [[nodiscard]] bool bound() const { return provider_ != nullptr; }

    [[nodiscard]] Codec<Made>& provider() const {
        if (provider_ == nullptr) throw Unbound{Made::kind()};

        return *provider_;
    }

    [[nodiscard]] Made decoded(const Bytes& given) const {
        Made made;

        if (const std::string said = provider().decode(given, made); !said.empty())
            throw Refused{Made::kind(), said};

        return made;
    }

    [[nodiscard]] Bytes encoded(const Made& given) const {
        Bytes made;

        if (const std::string said = provider().encode(given, made); !said.empty())
            throw Refused{Made::kind(), said};

        return made;
    }

private:
    std::shared_ptr<Codec<Made>> provider_;
};

}
