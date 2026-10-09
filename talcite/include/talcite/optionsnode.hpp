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

class HistoryField;

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

    [[nodiscard]] std::optional<std::size_t> find(const std::string& one) const;

    [[nodiscard]] const std::string& operator[](std::size_t which) const;

    [[nodiscard]] const std::string& front() const;

    [[nodiscard]] const std::string& back() const;

    using walk = std::vector<std::string>::const_iterator;

    [[nodiscard]] walk begin() const { return inside_.begin(); }

    [[nodiscard]] walk end() const { return inside_.end(); }

private:
    template <std::derived_from<Node> Base>
    friend class OptionsNode;

    friend class HistoryField;

    Options() = default;

    ~Options() = default;

    std::vector<std::string> inside_;
};

template <std::derived_from<Node> Base>
class OptionsNode : public Base {
public:
    [[nodiscard]] Options& options() { return options_; }

    [[nodiscard]] const Options& options() const { return options_; }

    [[nodiscard]] std::optional<std::size_t> which() const {
        if (options_.empty()) return {};

        return std::min(at_, options_.many() - 1);
    }

    void which(std::size_t given) {
        if (options_.empty()) return;

        at_ = std::min(given, options_.many() - 1);
    }

    [[nodiscard]] std::optional<std::string> value() const {
        if (options_.empty()) return {};

        return options_[std::min(at_, options_.many() - 1)];
    }

    void value(const std::string& one) {
        if (const std::optional<std::size_t> found = options_.find(one)) at_ = *found;
    }

    [[nodiscard]] int capacity() const override { return 0; }

protected:
    explicit OptionsNode(std::string role) { this->role(std::move(role)); }

private:
    Options options_;
    std::size_t at_ = 0;
};

}
