#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include "tal/executor.hpp"
#include "tal/path.hpp"
#include "tal/secrets.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

// The secrets of a program: the memory's for trials, and the system's — on systems with the Secret
// Service the whole way through a secret-tool of the trials' own, a script that keeps what it is
// given in its folder and writes down its arguments, so that no secret of the machine is touched.

namespace {

using tal::Kind;
using tal::Path;

// The kind of trouble `work` ended in, or "none".
template <typename T>
std::string kind_of(tal::Loop& loop, tal::Task<T> work) {
    try {
        (void)tal::wait(loop, std::move(work));
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())};
    }

    return "none";
}

#ifndef _WIN32

// The script that plays secret-tool: its arguments go to `args`; "store" keeps its input in the
// file of the key (the last argument), "lookup" prints it or fails, "clear" removes it.
constexpr const char* script = R"sh(#!/bin/sh
here=$(dirname "$0")
printf '%s|' "$@" >> "$here/args"
echo >> "$here/args"
for last; do :; done
case "$1" in
  store) cat > "$here/key-$last" ;;
  lookup) [ -f "$here/key-$last" ] || exit 1; cat "$here/key-$last" ;;
  clear) rm -f "$here/key-$last" ;;
esac
exit 0
)sh";

// A folder of a trial with its secret-tool, put back as it was after.
class Stage {
public:
    // Makes the folder and the script and points tal at it.
    Stage() : old_(tal::secreting::tool()) {
        static std::atomic<int> numbered{0};

        root_ = std::filesystem::temp_directory_path() / ("tal-secrets-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++numbered));
        std::filesystem::create_directories(root_);
        std::ofstream{root_ / "secret-tool", std::ios::binary} << script;
        std::filesystem::permissions(root_ / "secret-tool", std::filesystem::perms::owner_all);
        tal::secreting::tool() = Path{(root_ / "secret-tool").string()};
    }

    // Puts tal back and removes the folder.
    ~Stage() {
        tal::secreting::tool() = old_;

        std::error_code ignored;

        std::filesystem::remove_all(root_, ignored);
    }

    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    // The text of the file `name` of the folder, empty when there is none.
    [[nodiscard]] std::string got(const std::string& name) const {
        std::ifstream in{root_ / name, std::ios::binary};

        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

private:
    std::filesystem::path root_;
    Path old_;
};

#endif

}

// The memory keeps, gives back and forgets; refusing, it refuses all but forgetting.
TRIAL("Secrets: the memory's keep, give back and forget") {
    tal::Loop loop;
    tal::MemorySecrets secrets;

    SAME(tal::wait(loop, secrets.read("twa: TMDB")), std::optional<std::string>{});

    tal::wait(loop, secrets.write("twa: TMDB", "abc"));

    SAME(tal::wait(loop, secrets.read("twa: TMDB")), std::optional<std::string>{"abc"});

    tal::wait(loop, secrets.remove("twa: TMDB"));

    SAME(tal::wait(loop, secrets.read("twa: TMDB")), std::optional<std::string>{});

    secrets.refusing = true;

    SAME(kind_of(loop, secrets.write("twa: TMDB", "abc")), std::string{tal::spoken(Kind::refused)});
    SAME(kind_of(loop, secrets.read("twa: TMDB")), std::string{tal::spoken(Kind::refused)});
}

#ifndef _WIN32

// The Secret Service gets the secret on secret-tool's input, never in its arguments, and gives it
// back as it was kept; one not kept is none, and a cleared one too. Without secret-tool it is
// unsupported.
TRIAL("Secrets: the Secret Service through secret-tool, the secret on its input") {
    tal::Loop loop;
    const Stage stage;
    tal::Secrets& secrets = *tal::Secrets::builtin();

    SAME(std::string{secrets.store()}, std::string{"Secret Service"});
    SAME(tal::wait(loop, secrets.read("twa: TMDB")), std::optional<std::string>{});

    tal::wait(loop, secrets.write("twa: TMDB", "s3cret"));

    SAME(tal::wait(loop, secrets.read("twa: TMDB")), std::optional<std::string>{"s3cret"});
    OUGHT(stage.got("args").find("s3cret") == std::string::npos);
    OUGHT(stage.got("args").find("store|--label=twa: TMDB|service|tal|key|twa: TMDB|") != std::string::npos);

    tal::wait(loop, secrets.remove("twa: TMDB"));

    SAME(tal::wait(loop, secrets.read("twa: TMDB")), std::optional<std::string>{});

    tal::secreting::tool() = Path{"/no/such/secret-tool"};

    SAME(kind_of(loop, secrets.read("twa: TMDB")), std::string{tal::spoken(Kind::unsupported)});
    SAME(kind_of(loop, secrets.write("twa: TMDB", "x")), std::string{tal::spoken(Kind::unsupported)});
}

#endif
