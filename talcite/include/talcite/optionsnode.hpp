#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/node.hpp"

namespace talcite::ui {

class History;

class Options {
public:
    Options(const Options&) = delete;
    Options& operator=(const Options&) = delete;
    Options(Options&&) = delete;
    Options& operator=(Options&&) = delete;

    void add(std::string one);

    void insert(std::size_t at, std::string one);

    void remove(std::size_t which);

    void clear();

    void all(std::vector<std::string> given);

    [[nodiscard]] const std::vector<std::string>& all() const { return inside_; }

    [[nodiscard]] std::size_t many() const { return inside_.size(); }

    [[nodiscard]] bool empty() const { return inside_.empty(); }

    [[nodiscard]] bool contains(const std::string& one) const;

    [[nodiscard]] int which(const std::string& one) const;

    [[nodiscard]] const std::string& operator[](std::size_t which) const;

    [[nodiscard]] const std::string& front() const;

    [[nodiscard]] const std::string& back() const;

    using walk = std::vector<std::string>::const_iterator;

    [[nodiscard]] walk begin() const { return inside_.begin(); }

    [[nodiscard]] walk end() const { return inside_.end(); }

private:
    template <std::derived_from<Node> Above>
    friend class OptionsNode;

    friend class History;

    Options() = default;

    ~Options() = default;

    std::vector<std::string> inside_;
};

template <std::derived_from<Node> Above>
class OptionsNode : public Above {
public:
    [[nodiscard]] Options& options() { return options_; }

    [[nodiscard]] const Options& options() const { return options_; }

    [[nodiscard]] std::optional<std::string> which() const {
        if (options_.empty()) return {};

        return options_[std::min(at_, options_.many() - 1)];
    }

    void which(const std::string& one) {
        const int found = options_.which(one);

        if (found >= 0) at_ = static_cast<std::size_t>(found);
    }

    [[nodiscard]] bool able() const { return able_; }

    virtual void able(bool given) {
        if (able_ == given) return;

        able_ = given;

        if (able_) return;

        this->unfocus();
    }

    [[nodiscard]] int takes() const override { return 0; }

protected:
    explicit OptionsNode(std::string role) { this->role(std::move(role)); }

    [[nodiscard]] std::size_t at() const {
        return options_.empty() ? 0 : std::min(at_, options_.many() - 1);
    }

    void at(std::size_t given) {
        if (options_.empty()) return;

        at_ = std::min(given, options_.many() - 1);
    }

private:
    Options options_;
    std::size_t at_ = 0;
    bool able_ = true;
};

}
