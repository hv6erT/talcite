#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "tal/json.hpp"
#include "tal/text.hpp"
#include "tal/texts.hpp"
#include "tal/trouble.hpp"

#include "generating.hpp"
#include "wording.hpp"

// English of the target, with every shape of text: plain, with values, the letter of a key and
// a text for a number.
constexpr std::string_view english =
    R"({"meta":{"name":"English"},"title":"Trials","menu":{"left":{"text":"Left","key":"L"},"quit":"Quit"},)"
    R"("panel":{"marked":{"one":"{} file marked","other":"{} files marked"},"moved":"Moved {} to {}"}})";

// One input of libFuzzer as JSON, twice. As a translation it is checked against English in a
// language with many plural forms and in one without them; what `unfit` lets stand must have the
// values English has. As English, when the generator takes it, the dictionary must take it too and
// answer every key by its shape. Nothing may crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    tal::json::Value given;

    try {
        given = tal::json::parsed(text);
    } catch (const tal::Trouble&) {
        return 0;
    }

    const tal::json::Value base = tal::json::parsed(english);
    std::vector<tal::wording::Problem> ignored;

    for (const std::string_view code : {"pl-PL", "ja-JP"}) {
        const tal::text::Language language{code};

        (void)tal::wording::compared(given, base, language);

        for (const tal::wording::Entry& entry : tal::wording::entries(base, ignored)) {
            const tal::json::Value* found = tal::wording::found(given, entry.road);

            if (found == nullptr || !tal::wording::unfit(entry, *found, language).empty()) continue;

            if (entry.shape == tal::wording::Shape::counted) {
                for (const tal::text::Plural form : tal::text::plurals(language)) {
                    constexpr std::string_view names[] = {"zero", "one", "two", "few", "many", "other"};

                    if (tal::wording::slots(found->find(names[static_cast<std::size_t>(form)])->string()) != entry.slots) std::terminate();
                }
            } else if (tal::wording::slots(found->string()) != entry.slots) {
                std::terminate();
            }
        }
    }

    std::vector<std::string> problems;

    (void)tal::generating::generated({"en-US.json", std::string{text}}, "texts", "texts.hpp", problems);

    if (!problems.empty()) return 0;

    std::vector<tal::wording::Problem> shape;
    const std::vector<tal::wording::Entry> entries = tal::wording::entries(given, shape);
    std::vector<std::string> keys;

    for (const tal::wording::Entry& one : entries) keys.push_back(tal::wording::joined(one.road));

    const std::vector<std::string_view> viewed(keys.begin(), keys.end());
    const tal::Texts texts{viewed, tal::json::written(given)};

    for (std::size_t at = 0; at < entries.size(); ++at) {
        if (entries[at].shape == tal::wording::Shape::counted) (void)texts.counted(at, 7);
        else if (entries[at].shape == tal::wording::Shape::letter) (void)texts.letter(at);
        else (void)texts.said(at);
    }

    return 0;
}
