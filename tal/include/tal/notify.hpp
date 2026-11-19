#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/task.hpp"

namespace tal {

enum class Urgency { low, normal, critical };

struct Notice {
    std::string title;
    std::string body;
    Urgency urgency = Urgency::normal;

    friend bool operator==(const Notice&, const Notice&) = default;
};

class Notifier : public Adapter {
public:
    static constexpr std::string_view kind = "notifier";

    [[nodiscard]] static std::shared_ptr<Notifier> builtin();

    [[nodiscard]] virtual Task<void> tell(Notice notice) = 0;
};

[[nodiscard]] Task<void> tell(Notice notice, Port<Notifier> notifier = {});

[[nodiscard]] Port<Notifier> notifier(Port<Processes> processes);

class ScriptedNotifier : public Notifier {
public:
    std::vector<Notice> told;

    [[nodiscard]] std::string_view name() const override { return "scripted"; }

    [[nodiscard]] Task<void> tell(Notice notice) override;
};

}
