#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace talcite::detail {

template <typename What>
class Deadlines {
public:
    using Moment = std::chrono::steady_clock::time_point;

    struct Entry {
        Moment when;
        What what;
    };

    [[nodiscard]] std::uint64_t added(Moment when, What what) {
        const std::uint64_t which = next_ + 1;

        const auto placed = order_.emplace(when, which).first;

        try {
            all_.emplace(which, Entry{when, std::move(what)});
        } catch (...) {
            order_.erase(placed);

            throw;
        }

        next_ = which;

        return which;
    }

    [[nodiscard]] std::optional<Entry> taken(std::uint64_t which) {
        const auto at = all_.find(which);

        if (at == all_.end()) return std::nullopt;

        order_.erase({at->second.when, which});

        std::optional<Entry> made{std::move(at->second)};

        all_.erase(at);

        return made;
    }

    [[nodiscard]] std::optional<Entry> due(Moment now) {
        if (order_.empty() || order_.begin()->first > now) return std::nullopt;

        return taken(order_.begin()->second);
    }

    [[nodiscard]] std::vector<std::uint64_t> due_numbers(Moment now) const {
        std::vector<std::uint64_t> made;

        for (const auto& [when, which] : order_) {
            if (when > now) break;

            made.push_back(which);
        }

        return made;
    }

    [[nodiscard]] Entry* found(std::uint64_t which) {
        const auto at = all_.find(which);

        return at == all_.end() ? nullptr : &at->second;
    }

    void moved(std::uint64_t which, Moment when) {
        const auto at = all_.find(which);

        if (at == all_.end() || at->second.when == when) return;

        order_.emplace(when, which);
        order_.erase({at->second.when, which});

        at->second.when = when;
    }

    [[nodiscard]] std::optional<Moment> nearest() const {
        if (order_.empty()) return std::nullopt;

        return order_.begin()->first;
    }

    [[nodiscard]] bool earliest(std::uint64_t which) const { return !order_.empty() && order_.begin()->second == which; }

    [[nodiscard]] bool contains(std::uint64_t which) const { return all_.contains(which); }

    [[nodiscard]] bool empty() const { return all_.empty(); }

    [[nodiscard]] std::size_t size() const { return all_.size(); }

private:
    std::map<std::uint64_t, Entry> all_;
    std::set<std::pair<Moment, std::uint64_t>> order_;
    std::uint64_t next_ = 0;
};

}
