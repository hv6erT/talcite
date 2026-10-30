#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/child.hpp"
#include "talcite/console.hpp"
#include "talcite/fs.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"

namespace talcite {

enum class Runnable : std::uint8_t { no, window, console, script };

enum class Origin : std::uint8_t { here, internet };

enum class Started : std::uint8_t { opened, unassociated, sessionless, terminal };

struct Application {
    std::string name;
    std::string id;
    bool preferred = false;
    bool terminal = false;

    friend bool operator==(const Application&, const Application&) = default;
};

class Desktop : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "desktop"; }

    [[nodiscard]] virtual Promise<Started> open(fs::path file) = 0;

    [[nodiscard]] virtual Promise<std::vector<Application>> applications(fs::path file) = 0;

    [[nodiscard]] virtual Promise<Started> open_with(fs::path file, Application with) = 0;

    [[nodiscard]] virtual Promise<Started> open_by(fs::path file, fs::path program) = 0;

    [[nodiscard]] virtual Promise<std::optional<Command>> command(fs::path file, Application with) = 0;

    [[nodiscard]] virtual Promise<Runnable> runnable(fs::path file) = 0;

    [[nodiscard]] virtual Promise<Origin> origin(fs::path file) = 0;

    [[nodiscard]] virtual Promise<Started> chosen(fs::path file) = 0;
};

[[nodiscard]] Seam<Desktop> desktop();

[[nodiscard]] Seam<Desktop> desktop(Seam<Console> with);

}
