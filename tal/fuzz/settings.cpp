#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "tal/json.hpp"
#include "tal/settings.hpp"
#include "tal/trouble.hpp"

#include "generating.hpp"
#include "mending.hpp"

// The handles of the settings under `node` at `road`, typed as the generator types them, each asked
// for once.
void asked(const tal::Settings& settings, const tal::json::Value& node, const std::string& road) {
    for (std::size_t at = 0; at < node.size(); ++at) {
        const tal::json::Value& value = node.at(at);
        const std::string here = road.empty() ? node.names()[at] : road + "." + node.names()[at];

        if (value.kind() == tal::json::Kind::object) asked(settings, value, here);
        else if (road.empty()) (void)settings.get(tal::Setting<tal::Records>{here});
        else if (value.kind() == tal::json::Kind::boolean) (void)settings.get(tal::Setting<bool>{here});
        else if (value.kind() == tal::json::Kind::integer) (void)settings.get(tal::Setting<int>{here});
        else if (value.kind() == tal::json::Kind::string) (void)settings.get(tal::Setting<std::string>{here});
        else (void)settings.get(tal::Setting<std::vector<std::string>>{here});
    }
}

// One input of libFuzzer as a settings file mended by hand and as built-in values. The mending
// may make any text, but never one more than twice as long and four bytes, and its JSON either
// reads or is refused with `Trouble`. Built-in values the generator takes, the settings take too,
// and every handle of them answers. Nothing may crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    const std::string mended = tal::mended(text);

    if (mended.size() > 2 * text.size() + 4) std::terminate();

    try {
        (void)tal::json::parsed(mended);
    } catch (const tal::Trouble&) {
    }

    std::vector<std::string> problems;

    (void)tal::generating::settled({"defaults.json", std::string{text}}, "settings", "settings.hpp", problems);

    if (!problems.empty()) return 0;

    const tal::json::Value document = tal::json::parsed(text);
    const tal::Settings settings{tal::json::written(document), tal::Path{"/settings.json"}};

    asked(settings, document, "");

    return 0;
}
