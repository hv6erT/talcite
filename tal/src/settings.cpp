#include "tal/settings.hpp"

#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/trouble.hpp"

#include "mending.hpp"

// A program's settings: the values built into the program underneath, and above them a layer of
// the person's own, kept in a file. The two never merge, so the file holds only what differs from
// the built-in values, and a default changed in a new version reaches everyone who did not set the
// value themselves; hence `changed`, and `reset` as "drop the deviation", not "write the default".
// A setting is asked for by a handle with its type (`Setting`), which the generator of settings
// makes from the built-in values, so a wrong road or a wrong type does not compile. The file holds
// keys to services, so it is never readable by anyone but its owner, nor is its copy.
namespace tal {

namespace {

// The rights of the file and of its copy: read and write by the owner alone.
constexpr std::uint16_t owner_only = 0600;

// The ending of the copy a repair or a restore keeps of the original.
constexpr std::string_view sheltered_ending = ".broken";

// The road `road` split at its points.
std::vector<std::string_view> split(std::string_view road) {
    std::vector<std::string_view> made;

    for (std::size_t from = 0;;) {
        const std::size_t to = road.find('.', from);

        made.push_back(road.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from));

        if (to == std::string_view::npos) return made;

        from = to + 1;
    }
}

// The value at `road` in `document`, or none when a step is missing or not an object.
const json::Value* walked(const json::Value& document, std::string_view road) {
    const json::Value* here = &document;

    for (const std::string_view name : split(road)) {
        if (here->kind() != json::Kind::object) return nullptr;

        here = here->find(name);

        if (here == nullptr) return nullptr;
    }

    return here;
}

// `node` with `leaf` at the road `parts` from `at` on; what stands in the way and is not an object
// becomes one, since the new value knows what it wants.
json::Value with(json::Value node, const std::vector<std::string_view>& parts, std::size_t at, json::Value leaf) {
    if (node.kind() != json::Kind::object) node = json::Value::object();

    if (at + 1 == parts.size()) {
        node.put(std::string{parts[at]}, std::move(leaf));

        return node;
    }

    const json::Value* below = node.find(parts[at]);

    node.put(std::string{parts[at]}, with(below != nullptr ? *below : json::Value::object(), parts, at + 1, std::move(leaf)));

    return node;
}

// `node` without the value at the road `parts` from `at` on, and without the objects that became
// empty because of it; none when `node` itself became empty. A road that is not there changes
// nothing.
std::optional<json::Value> without(const json::Value& node, const std::vector<std::string_view>& parts, std::size_t at) {
    if (node.kind() != json::Kind::object || node.find(parts[at]) == nullptr) return node;

    json::Value made = json::Value::object();

    for (std::size_t which = 0; which < node.size(); ++which) {
        const std::string& name = node.names()[which];

        if (name != parts[at]) {
            made.put(name, node.at(which));
        } else if (at + 1 < parts.size()) {
            if (std::optional<json::Value> kept = without(node.at(which), parts, at + 1)) made.put(name, std::move(*kept));
        }
    }

    if (made.size() == 0) return std::nullopt;

    return made;
}

// The value of `from` as the type T, or none when `from` has another shape. A whole number with a
// fraction is cut to the whole, one outside `int` has another shape; a list of texts holds texts
// only, and a list of records objects of texts only: half a list is not a list.
template <typename T>
std::optional<T> taken(const json::Value& from);

// A boolean.
template <>
std::optional<bool> taken<bool>(const json::Value& from) {
    if (from.kind() != json::Kind::boolean) return std::nullopt;

    return from.boolean();
}

// A whole number within `int`, a number with a fraction cut toward zero.
template <>
std::optional<int> taken<int>(const json::Value& from) {
    if (from.kind() == json::Kind::integer) {
        const std::int64_t one = from.integer();

        if (one < std::numeric_limits<int>::min() || one > std::numeric_limits<int>::max()) return std::nullopt;

        return static_cast<int>(one);
    }

    if (from.kind() != json::Kind::number) return std::nullopt;

    const double one = from.number();

    if (!(one > static_cast<double>(std::numeric_limits<int>::min()) - 1.0 && one < static_cast<double>(std::numeric_limits<int>::max()) + 1.0))
        return std::nullopt;

    return static_cast<int>(one);
}

// A text.
template <>
std::optional<std::string> taken<std::string>(const json::Value& from) {
    if (from.kind() != json::Kind::string) return std::nullopt;

    return from.string();
}

// A list of texts.
template <>
std::optional<std::vector<std::string>> taken<std::vector<std::string>>(const json::Value& from) {
    if (from.kind() != json::Kind::array) return std::nullopt;

    std::vector<std::string> made;

    for (std::size_t at = 0; at < from.size(); ++at) {
        if (from.at(at).kind() != json::Kind::string) return std::nullopt;

        made.push_back(from.at(at).string());
    }

    return made;
}

// A list of records, each an object of texts.
template <>
std::optional<Records> taken<Records>(const json::Value& from) {
    if (from.kind() != json::Kind::array) return std::nullopt;

    Records made;

    for (std::size_t at = 0; at < from.size(); ++at) {
        const json::Value& one = from.at(at);

        if (one.kind() != json::Kind::object) return std::nullopt;

        std::map<std::string, std::string> row;

        for (std::size_t field = 0; field < one.size(); ++field) {
            if (one.at(field).kind() != json::Kind::string) return std::nullopt;

            row[one.names()[field]] = one.at(field).string();
        }

        made.push_back(std::move(row));
    }

    return made;
}

// A boolean as JSON.
json::Value written(bool given) { return json::Value{given}; }

// A whole number as JSON.
json::Value written(int given) { return json::Value{given}; }

// A text as JSON.
json::Value written(const std::string& given) { return json::Value{given}; }

// A list of texts as JSON.
json::Value written(const std::vector<std::string>& given) {
    json::Value made = json::Value::array();

    for (const std::string& one : given) made.push(json::Value{one});

    return made;
}

// A list of records as JSON, the fields of each in their (alphabetical) order.
json::Value written(const Records& given) {
    json::Value made = json::Value::array();

    for (const std::map<std::string, std::string>& row : given) {
        json::Value one = json::Value::object();

        for (const auto& [name, text] : row) one.put(name, json::Value{text});

        made.push(std::move(one));
    }

    return made;
}

// The text of the file at `where`, at most one byte longer than JSON may be, so that a longer one
// is refused by the reader rather than read whole into memory.
Task<std::string> fetched(Path where) { co_return text::of(co_await fs::read(where, fs::Range{0, json::Limits{}.bytes + 1})); }

// Keeps the original `given` beside the file at `where`, under the ending `.broken`, with the
// rights of the file itself: the original holds keys to services, and a file edited by hand is
// often readable by everyone.
Task<void> sheltered(Path where, Bytes given) {
    const Path copy = where.parent() / (std::string{where.name()} + std::string{sheltered_ending});

    co_await fs::write(copy, std::move(given), fs::Writing{.rights = owner_only});
}

// Writes `bytes` to the file at `where` after the write `before` ended, well or not, so that the
// writes of the settings go one after another in the order they were asked for. A file that is a
// symbolic link is written in its target, so the link stays.
Task<void> stored(std::shared_ptr<Shared<void>> before, Path where, Bytes bytes) {
    if (before) {
        try {
            co_await before->get();
        } catch (const Cancelled&) {
            throw;
        } catch (...) {
        }

        before.reset();
    }

    Path target = where;

    try {
        target = co_await fs::canonical(where);
    } catch (const Trouble&) {
    }

    co_await fs::make(target.parent());
    co_await fs::write(target, std::move(bytes), fs::Writing{.rights = owner_only});
}

// A task that fails with `why`.
Task<void> failing(std::exception_ptr why) {
    std::rethrow_exception(why);

    co_return;
}

// A task that waits for `write`.
Task<void> awaited(std::shared_ptr<Shared<void>> write) { co_await write->get(); }

}

// The last write asked for, which the next one waits for.
struct Settings::Gate {
    std::shared_ptr<Shared<void>> last;
};

// Settings with the built-in values `builtin`, from the generator of settings, and the file at
// `file`, not read yet. Built-in values that are not a JSON object are a mistake of the program.
Settings::Settings(std::string_view builtin, Path file)
    : builtin_(json::parsed(builtin)), mine_(json::Value::object()), where_(std::move(file)), gate_(std::make_shared<Gate>()) {
    if (builtin_.kind() != json::Kind::object) throw std::logic_error{"tal::Settings: the built-in values are not an object"};
}

Settings::~Settings() = default;

// Starts over from the file: the deviations it holds replace those held now. The pool reads, and
// the layer changes at once at the end, where the caller is. A missing file, or a folder in its
// place, is a first run; a file that cannot be read or is not a JSON object is damaged (`damaged`),
// and the settings run on the built-in values until the person repairs or restores it.
Task<void> Settings::read() {
    Executor& back = co_await detail::Here{};

    co_await to(pool());

    bool present = false;
    std::optional<json::Value> layer;

    try {
        present = (co_await fs::look(where_)).reach == fs::Type::file;
    } catch (const Trouble&) {
    }

    if (present) {
        try {
            json::Value made = json::parsed(co_await fetched(where_));

            if (made.kind() == json::Kind::object) layer = std::move(made);
        } catch (const Trouble&) {
        }
    }

    co_await to(back);

    mine_ = layer ? std::move(*layer) : json::Value::object();
    damaged_ = present && !layer;
}

// Writes the deviations, as they are at the call, to the file, which only its owner can read; the
// writes go one after another. A damaged file stays as it is until the person decides, so writing
// refuses: one typo in the file and a change of sorting would otherwise overwrite the person's
// whole file, keys included.
Task<void> Settings::write() const {
    if (damaged_) return failing(std::make_exception_ptr(Trouble{Kind::refused, "The settings file is damaged and stays untouched.", where_.text()}));

    Bytes bytes;

    try {
        bytes = tal::bytes(json::written(mine_, true) + "\n");
    } catch (...) {
        return failing(std::current_exception());
    }

    const auto made = std::make_shared<Shared<void>>(stored(gate_->last, where_, std::move(bytes)));

    gate_->last = made;

    return awaited(made);
}

// Repairs the file after hand edits (`mended`): comments, commas before a bracket and a cut-off end
// go; what is still not JSON is not guessed, and the file stays as it was, with a refusal. A
// repaired file keeps its original beside it (`.broken`), and its deviations are written anew.
Task<void> Settings::repair() {
    Executor& back = co_await detail::Here{};

    co_await to(pool());

    const std::string given = co_await fetched(where_);
    json::Value made;

    try {
        made = json::parsed(mended(given));
    } catch (const Trouble& why) {
        throw Trouble{Kind::malformed, std::string{"The settings file cannot be repaired: "} + why.what(), where_.text()};
    }

    if (made.kind() != json::Kind::object) throw Trouble{Kind::malformed, "The settings file cannot be repaired: not an object.", where_.text()};

    co_await sheltered(where_, tal::bytes(given));
    co_await to(back);

    mine_ = std::move(made);
    damaged_ = false;

    co_await write();
}

// Starts again from the built-in values: the original stays beside the file (`.broken`), since the
// keys to services go too, and the empty layer is written.
Task<void> Settings::restore() {
    Executor& back = co_await detail::Here{};

    co_await to(pool());
    co_await sheltered(where_, tal::bytes(co_await fetched(where_)));
    co_await to(back);

    mine_ = json::Value::object();
    damaged_ = false;

    co_await write();
}

// The value of `one`: the person's when it has the setting's type, otherwise the built-in one; a
// value of another shape in the file gives way silently, since a person writes the file.
template <typename T>
T Settings::get(Setting<T> one) const {
    if (const json::Value* mine = walked(mine_, one.road))
        if (std::optional<T> made = taken<T>(*mine)) return *std::move(made);

    return builtin(one);
}

// The built-in value of `one`. A setting the built-in values lack, or have of another type, is a
// mistake of the program: the handles come from the same values.
template <typename T>
T Settings::builtin(Setting<T> one) const {
    const json::Value* found = walked(builtin_, one.road);

    if (found == nullptr) throw std::logic_error{"tal::Settings: no built-in setting " + std::string{one.road}};

    std::optional<T> made = taken<T>(*found);

    if (!made) throw std::logic_error{"tal::Settings: the built-in setting " + std::string{one.road} + " has another type"};

    return *std::move(made);
}

// Sets the person's value of `one`, which then differs from the built-in one even when equal to it.
template <typename T>
void Settings::set(Setting<T> one, const T& value) {
    mine_ = with(std::move(mine_), split(one.road), 0, written(value));
}

// Drops the person's value of `one`, with the objects it leaves empty.
template <typename T>
void Settings::reset(Setting<T> one) {
    std::optional<json::Value> kept = without(mine_, split(one.road), 0);

    mine_ = kept ? std::move(*kept) : json::Value::object();
}

// Whether the person has a value of `one`, of whatever shape.
template <typename T>
bool Settings::changed(Setting<T> one) const {
    return walked(mine_, one.road) != nullptr;
}

// Drops all the person's values; keys the program does not know go too.
void Settings::reset() { mine_ = json::Value::object(); }

template bool Settings::get(Setting<bool>) const;
template int Settings::get(Setting<int>) const;
template std::string Settings::get(Setting<std::string>) const;
template std::vector<std::string> Settings::get(Setting<std::vector<std::string>>) const;
template Records Settings::get(Setting<Records>) const;

template bool Settings::builtin(Setting<bool>) const;
template int Settings::builtin(Setting<int>) const;
template std::string Settings::builtin(Setting<std::string>) const;
template std::vector<std::string> Settings::builtin(Setting<std::vector<std::string>>) const;
template Records Settings::builtin(Setting<Records>) const;

template void Settings::set(Setting<bool>, const bool&);
template void Settings::set(Setting<int>, const int&);
template void Settings::set(Setting<std::string>, const std::string&);
template void Settings::set(Setting<std::vector<std::string>>, const std::vector<std::string>&);
template void Settings::set(Setting<Records>, const Records&);

template void Settings::reset(Setting<bool>);
template void Settings::reset(Setting<int>);
template void Settings::reset(Setting<std::string>);
template void Settings::reset(Setting<std::vector<std::string>>);
template void Settings::reset(Setting<Records>);

template bool Settings::changed(Setting<bool>) const;
template bool Settings::changed(Setting<int>) const;
template bool Settings::changed(Setting<std::string>) const;
template bool Settings::changed(Setting<std::vector<std::string>>) const;
template bool Settings::changed(Setting<Records>) const;

}
