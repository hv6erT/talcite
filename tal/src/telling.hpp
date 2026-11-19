#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "tal/console.hpp"
#include "tal/executor.hpp"
#include "tal/notify.hpp"
#include "tal/port.hpp"
#include "tal/task.hpp"

namespace tal::telling {

enum class Way { none, nine, notify, kitty };

[[nodiscard]] Way way(std::string_view named);

[[nodiscard]] std::string shown(std::string_view text, bool lines);

[[nodiscard]] std::string sequence(const Notice& notice, Way how, std::string_view id);

class Terminal final : public Notifier {
public:
    Terminal(Port<Console> console, Way how, std::shared_ptr<Notifier> otherwise, Executor& loop);

    [[nodiscard]] std::string_view name() const override { return "terminal"; }

    [[nodiscard]] Task<void> tell(Notice notice) override;

    void closed();

    void away(bool aside);

    [[nodiscard]] std::shared_ptr<Notifier> otherwise() const;

private:
    struct Shared;

    std::shared_ptr<Shared> shared_;
};

}
