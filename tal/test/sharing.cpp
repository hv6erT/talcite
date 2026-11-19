#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tal/executor.hpp"
#include "tal/path.hpp"
#include "tal/share.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

#include "sharing.hpp"

// Network shares: the addresses and the names of GVfs, the reasons of gio and of Windows, and on
// systems with GVfs the whole way through a gio of the trials' own, a script that writes down what
// it was asked and answers what its folder tells it. No share of any network is touched.

namespace {

using tal::Kind;
using tal::Path;
using tal::fs::Share;

namespace sharing = tal::fs::sharing;

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

// The kind `kind` as spoken.
std::string named(Kind kind) { return std::string{tal::spoken(kind)}; }

// The share `one` as people write it, or "none".
std::string shown(const std::optional<Share>& one) {
    if (!one) return "none";

    return one->server + "|" + one->name + "|" + one->rest + "|" + one->user;
}

#ifndef _WIN32

// The script that plays gio: it writes its arguments and LC_ALL to `args`, its input to `input`,
// makes the mount named in `mount` when asked to mount, and prints `out-<command>` and
// `err-<command>`, ending with `code-<command>`.
constexpr const char* script = R"sh(#!/bin/sh
here=$(dirname "$0")
printf '%s|' "$@" >> "$here/args"
echo "LC_ALL=$LC_ALL" >> "$here/args"
cat >> "$here/input"
if [ "$1" = mount ] && [ -f "$here/mount" ]; then mkdir -p "$XDG_RUNTIME_DIR/gvfs/$(cat "$here/mount")"; fi
if [ -f "$here/out-$1" ]; then cat "$here/out-$1"; fi
if [ -f "$here/err-$1" ]; then cat "$here/err-$1" >&2; fi
if [ -f "$here/code-$1" ]; then exit "$(cat "$here/code-$1")"; fi
exit 0
)sh";

// A folder of a trial with its gio and its XDG_RUNTIME_DIR, both put back as they were after.
class Stage {
public:
    // Makes the folder, the script and run/gvfs, and points tal and the environment at them.
    Stage() : old_gio_(sharing::gio()) {
        static std::atomic<int> numbered{0};

        root_ = std::filesystem::temp_directory_path() /
                ("tal-shares-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++numbered));
        std::filesystem::create_directories(root_ / "run" / "gvfs");
        put("gio", script);
        std::filesystem::permissions(root_ / "gio", std::filesystem::perms::owner_all);

        if (const char* old = std::getenv("XDG_RUNTIME_DIR"); old != nullptr) old_runtime_ = old;

        (void)setenv("XDG_RUNTIME_DIR", (root_ / "run").c_str(), 1);
        sharing::gio() = Path{(root_ / "gio").string()};
    }

    // Puts tal and the environment back and removes the folder.
    ~Stage() {
        sharing::gio() = old_gio_;

        if (old_runtime_) (void)setenv("XDG_RUNTIME_DIR", old_runtime_->c_str(), 1);
        else (void)unsetenv("XDG_RUNTIME_DIR");

        std::error_code ignored;

        std::filesystem::remove_all(root_, ignored);
    }

    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    // Writes `text` to the file `name` of the folder.
    void put(const std::string& name, const std::string& text) const { std::ofstream{root_ / name, std::ios::binary} << text; }

    // The text of the file `name` of the folder, empty when there is none.
    [[nodiscard]] std::string got(const std::string& name) const {
        std::ifstream in{root_ / name, std::ios::binary};

        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }

    // Makes the mount `name` in run/gvfs, as GVfs would, and the folders `inside` it.
    [[nodiscard]] Path mounted(const std::string& name, const std::string& inside = {}) const {
        std::filesystem::create_directories(root_ / "run" / "gvfs" / name / inside);

        return Path{(root_ / "run" / "gvfs" / name).string()};
    }

    // The folder of GVfs.
    [[nodiscard]] Path gvfs() const { return Path{(root_ / "run" / "gvfs").string()}; }

private:
    std::filesystem::path root_;
    Path old_gio_;
    std::optional<std::string> old_runtime_;
};

#endif

}

TRIAL("Share: an address in UNC, // or smb:// form is a server, a share, the rest and a user") {
    SAME(shown(tal::fs::addressed("\\\\NAS\\films\\kino\\a.mkv")), std::string{"NAS|films|kino/a.mkv|"});
    SAME(shown(tal::fs::addressed("//NAS/films")), std::string{"NAS|films||"});
    SAME(shown(tal::fs::addressed("  smb://NAS/films/  ")), std::string{"NAS|films||"});
    SAME(shown(tal::fs::addressed("SMB://hubert@192.168.1.10/films/a%20b")), std::string{"192.168.1.10|films|a b|hubert"});
    SAME(shown(tal::fs::addressed("smb://hubert:sekret@nas/films")), std::string{"nas|films||hubert"});
    SAME(shown(tal::fs::addressed("smb://do%3Bm@nas/f%zz")), std::string{"nas|f%zz||do;m"});
    SAME(shown(tal::fs::addressed("\\\\NAS")), std::string{"NAS|||"});
    SAME(shown(tal::fs::addressed("//NAS//films///a%20b")), std::string{"NAS|films|a%20b|"});
    SAME(shown(tal::fs::addressed("smb://nas/k%c4%85t/a%4")), std::string{"nas|kąt|a%4|"});
    SAME(shown(tal::fs::addressed("smb://a@b.pl@nas/films/x@y")), std::string{"nas|films|x@y|a@b.pl"});
    SAME(shown(tal::fs::addressed("smb://nas/films/x@y")), std::string{"nas|films|x@y|"});
    SAME(shown(tal::fs::addressed("\\\\user@NAS\\films")), std::string{"user@NAS|films||"});
    SAME(shown(tal::fs::addressed("smb://")), std::string{"none"});
    SAME(shown(tal::fs::addressed("//")), std::string{"none"});
    SAME(shown(tal::fs::addressed("C:\\films")), std::string{"none"});
    SAME(shown(tal::fs::addressed("/films")), std::string{"none"});
    SAME(shown(tal::fs::addressed("http://nas/films")), std::string{"none"});

#ifdef _WIN32
    SAME(tal::fs::written(Share{"NAS", "films", "kino/a.mkv", "hubert"}), std::string{"\\\\NAS\\films\\kino\\a.mkv"});
    SAME(tal::fs::written(Share{"NAS", "", "", ""}), std::string{"\\\\NAS"});
    SAME(shown(tal::fs::shared(Path{"\\\\NAS\\films\\kino"})), std::string{"NAS|films|kino|"});
    SAME(shown(tal::fs::shared(Path{"C:\\films"})), std::string{"none"});
#else
    SAME(tal::fs::written(Share{"NAS", "films", "kino/a.mkv", "hubert"}), std::string{"smb://hubert@NAS/films/kino/a.mkv"});
    SAME(tal::fs::written(Share{"NAS", "", "", ""}), std::string{"smb://NAS"});
    SAME(shown(tal::fs::addressed(tal::fs::written(Share{"NAS", "films", "kino/a.mkv", "hubert"}))), std::string{"NAS|films|kino/a.mkv|hubert"});
#endif
}

TRIAL("Share: the folders of GVfs name their shares; a mount is found in any case, its user's first") {
    SAME(shown(sharing::gvfs_named("smb-share:server=nas,share=films")), std::string{"nas|films||"});
    SAME(shown(sharing::gvfs_named("smb-share:domain=dom,server=nas,share=fil%20my,user=hubert")), std::string{"nas|fil my||hubert"});
    SAME(shown(sharing::gvfs_named("smb-share:server=nas")), std::string{"none"});
    SAME(shown(sharing::gvfs_named("smb-share:share=films")), std::string{"none"});
    SAME(shown(sharing::gvfs_named("smb-share:server=nas,odd,share=films")), std::string{"nas|films||"});
    SAME(shown(sharing::gvfs_named("smb-server:server=nas")), std::string{"none"});
    SAME(shown(sharing::gvfs_named("sftp:host=nas")), std::string{"none"});

    const std::vector<std::string> names{"sftp:host=nas", "smb-share:server=nas,share=films,user=gosc", "smb-share:server=nas,share=films,user=hubert",
                                         "smb-share:server=nas,share=films", "smb-share:server=nas,share=muzyka"};

    SAME(sharing::mounted_as(names, Share{"NAS", "Films", "", ""}).value_or("none"), std::string{"smb-share:server=nas,share=films,user=gosc"});
    SAME(sharing::mounted_as(names, Share{"NAS", "Films", "", "hubert"}).value_or("none"), std::string{"smb-share:server=nas,share=films,user=hubert"});
    SAME(sharing::mounted_as(names, Share{"NAS", "Films", "", "obcy"}).value_or("none"), std::string{"smb-share:server=nas,share=films,user=gosc"});
    SAME(sharing::mounted_as(names, Share{"nas", "obrazy", "", ""}).value_or("none"), std::string{"none"});
    SAME(sharing::mounted_as(names, Share{"inny", "films", "", ""}).value_or("none"), std::string{"none"});
}

TRIAL("Share: gio gets the user, the domain and the password a line each; its list hides the shares with $") {
    SAME(sharing::answers("hubert", "sekret"), std::string{"hubert\n\nsekret\n"});
    SAME(sharing::answers("DOM\\hubert", "s"), std::string{"hubert\nDOM\ns\n"});
    SAME(sharing::answers("DOM;hubert", ""), std::string{"hubert\nDOM\n\n"});

    SAME(sharing::listed(" films/\nIPC$/\nmuzyka\n\nprint$\n  zdjęcia  \r\n"), (std::vector<std::string>{"films", "muzyka", "zdjęcia"}));
    SAME(sharing::listed(""), std::vector<std::string>{});
    SAME(sharing::listed("/\n$"), std::vector<std::string>{});
}

TRIAL("Share: the reasons of gio and of Windows are kinds of trouble, the network's before the login's") {
    SAME(named(sharing::by_gio("Failed to retrieve share list from server: Connection timed out")), named(Kind::broken));
    SAME(named(sharing::by_gio("Connection refused")), named(Kind::broken));
    SAME(named(sharing::by_gio("Authentication failed: Connection timed out")), named(Kind::broken));
    SAME(named(sharing::by_gio("Permission denied: no such share")), named(Kind::denied));
    SAME(named(sharing::by_gio("Share not found: operation not supported")), named(Kind::missing));
    SAME(named(sharing::by_gio("No route to host")), named(Kind::broken));
    SAME(named(sharing::by_gio("Network is unreachable")), named(Kind::broken));
    SAME(named(sharing::by_gio("Host is down")), named(Kind::broken));
    SAME(named(sharing::by_gio("Name or service not known")), named(Kind::broken));
    SAME(named(sharing::by_gio("Could not resolve hostname")), named(Kind::broken));
    SAME(named(sharing::by_gio("Unable to resolve host")), named(Kind::broken));
    SAME(named(sharing::by_gio("Host is unreachable")), named(Kind::broken));
    SAME(named(sharing::by_gio("gio: smb://nas/films/: Failed to mount Windows share: Permission denied")), named(Kind::denied));
    SAME(named(sharing::by_gio("Access denied")), named(Kind::denied));
    SAME(named(sharing::by_gio("Password required")), named(Kind::denied));
    SAME(named(sharing::by_gio("Authentication failed")), named(Kind::denied));
    SAME(named(sharing::by_gio("NT_STATUS_LOGON_FAILURE: logon failure")), named(Kind::denied));
    SAME(named(sharing::by_gio("Operation was cancelled")), named(Kind::denied));
    SAME(named(sharing::by_gio("Operation was canceled")), named(Kind::denied));
    SAME(named(sharing::by_gio("The operation was aborted")), named(Kind::denied));
    SAME(named(sharing::by_gio("No such file or directory")), named(Kind::missing));
    SAME(named(sharing::by_gio("Share not found")), named(Kind::missing));
    SAME(named(sharing::by_gio("Location does not exist")), named(Kind::missing));
    SAME(named(sharing::by_gio("no such share")), named(Kind::missing));
    SAME(named(sharing::by_gio("Operation not supported")), named(Kind::unsupported));
    SAME(named(sharing::by_gio("unsupported")), named(Kind::unsupported));
    SAME(named(sharing::by_gio("Location is not mounted")), named(Kind::unsupported));
    SAME(named(sharing::by_gio("No such mount")), named(Kind::unsupported));
    SAME(named(sharing::by_gio("Backend does not implement mount")), named(Kind::unsupported));
    SAME(named(sharing::by_gio("Something odd")), named(Kind::other));

    for (const std::uint32_t code : {5U, 86U, 1244U, 1326U, 1327U, 1330U, 1331U, 1385U, 2202U}) SAME(named(sharing::by_windows(code)), named(Kind::denied));
    for (const std::uint32_t code : {67U, 2310U}) SAME(named(sharing::by_windows(code)), named(Kind::missing));
    for (const std::uint32_t code : {51U, 53U, 59U, 64U, 121U, 1203U, 1222U, 1231U, 1232U}) SAME(named(sharing::by_windows(code)), named(Kind::broken));
    for (const std::uint32_t code : {50U, 1200U, 1204U}) SAME(named(sharing::by_windows(code)), named(Kind::unsupported));

    SAME(named(sharing::by_windows(1219)), named(Kind::exists));
    SAME(named(sharing::by_windows(0)), named(Kind::other));
    SAME(named(sharing::by_windows(87)), named(Kind::other));
}

TRIAL("Share: a share without a server or a name is the caller's error; remembering waits for TAL_REMEMBER_LOGINS") {
    tal::Loop loop;

    SAME(kind_of(loop, tal::fs::reach(Share{"", "films", "", ""})), named(Kind::other));
    SAME(kind_of(loop, tal::fs::reach(Share{"nas", "", "", ""})), named(Kind::other));
    SAME(kind_of(loop, tal::fs::connect(Share{"", "films", "", ""}, "hubert", "sekret")), named(Kind::other));
    SAME(kind_of(loop, tal::fs::shares(Share{"", "films", "", ""})), named(Kind::other));

#if defined(_WIN32) && defined(TAL_REMEMBER_LOGINS)
    OUGHT(tal::fs::keeps_logins());
#else
    OUGHT(!tal::fs::keeps_logins());
#endif
}

#ifdef _WIN32

TRIAL("Share: Windows: a share this computer has not is missing; its own shares are listed, sorted and without $") {
    tal::Loop loop;
    const Share absent{"localhost", "tal-no-such-share-of-the-trials", "a", ""};

    try {
        (void)tal::wait(loop, tal::fs::reach(absent));
        OUGHT(false);
    } catch (const tal::Trouble& why) {
        OUGHT(why.kind() == Kind::missing || why.kind() == Kind::broken);
        SAME(why.where(), std::string{"\\\\localhost\\tal-no-such-share-of-the-trials\\a"});
    }

    try {
        const std::vector<std::string> names = tal::wait(loop, tal::fs::shares(Share{"localhost", "", "", ""}));

        OUGHT(std::is_sorted(names.begin(), names.end()));

        for (const std::string& one : names) OUGHT(!one.empty() && one.back() != '$');
    } catch (const tal::Trouble& why) {
        OUGHT(why.kind() != Kind::other);
    }
}

#endif

#ifndef _WIN32

TRIAL("Share: GVfs: a share mounted already is reached without gio, with the rest; shared() reads it back") {
    tal::Loop loop;
    const Stage stage;
    const Path films = stage.mounted("smb-share:server=nas,share=films,user=hubert", "kino");

    (void)stage.mounted("smb-share:server=nas,share=films,user=gosc");

    SAME(tal::wait(loop, tal::fs::reach(Share{"NAS", "FILMS", "kino", "hubert"})), films / "kino");
    SAME(stage.got("args"), std::string{});

    SAME(shown(tal::fs::shared(films / "kino" / "a.mkv")), std::string{"nas|films|kino/a.mkv|hubert"});
    SAME(shown(tal::fs::shared(films)), std::string{"nas|films||hubert"});
    SAME(shown(tal::fs::shared(stage.gvfs())), std::string{"none"});
    SAME(shown(tal::fs::shared(stage.gvfs() / "sftp:host=nas" / "a")), std::string{"none"});
    SAME(shown(tal::fs::shared(Path{"/films/a"})), std::string{"none"});
}

TRIAL("Share: GVfs: one not mounted is mounted by gio in English and found; gio's failure is its kind") {
    tal::Loop loop;
    const Stage stage;

    stage.put("mount", "smb-share:server=nas,share=films");

    SAME(tal::wait(loop, tal::fs::reach(Share{"nas", "films", "a/b", ""})), stage.gvfs() / "smb-share:server=nas,share=films" / "a" / "b");
    SAME(stage.got("args"), std::string{"mount|smb://nas/films/|LC_ALL=C\n"});
    SAME(stage.got("input"), std::string{});

    stage.put("args", "");
    stage.put("code-mount", "2");
    stage.put("err-mount", "gio: smb://nas/muzyka/: Failed to mount Windows share: Permission denied\n");

    SAME(kind_of(loop, tal::fs::reach(Share{"nas", "muzyka", "", ""})), named(Kind::denied));

    try {
        (void)tal::wait(loop, tal::fs::reach(Share{"nas", "muzyka", "", ""}));
    } catch (const tal::Trouble& why) {
        SAME(std::string{why.what()}, std::string{"gio: smb://nas/muzyka/: Failed to mount Windows share: Permission denied"});
        SAME(std::string{why.where()}, std::string{"smb://nas/muzyka"});
    }

    stage.put("err-mount", "gio: smb://nas/muzyka/: Location is already mounted\n");

    SAME(kind_of(loop, tal::fs::reach(Share{"nas", "muzyka", "", ""})), named(Kind::unsupported));

    stage.put("err-mount", "");

    try {
        (void)tal::wait(loop, tal::fs::reach(Share{"nas", "muzyka", "", ""}));
    } catch (const tal::Trouble& why) {
        SAME(named(why.kind()), named(Kind::other));
        SAME(std::string{why.what()}, std::string{"The share cannot be reached."});
    }
}

TRIAL("Share: GVfs: a login goes to gio's input, never its arguments; remembering is refused before gio") {
    tal::Loop loop;
    const Stage stage;

    stage.put("mount", "smb-share:server=nas,share=films,user=hubert");

    SAME(kind_of(loop, tal::fs::connect(Share{"nas", "films", "", ""}, "hubert", "sekret", true)), named(Kind::other));
    SAME(stage.got("args"), std::string{});

    SAME(tal::wait(loop, tal::fs::connect(Share{"nas", "films", "", "hubert"}, "DOM\\hubert", "sekret")), stage.gvfs() / "smb-share:server=nas,share=films,user=hubert");
    SAME(stage.got("args"), std::string{"mount|smb://nas/films/|LC_ALL=C\n"});
    SAME(stage.got("input"), std::string{"hubert\nDOM\nsekret\n"});

    stage.put("args", "");
    stage.put("input", "");

    SAME(tal::wait(loop, tal::fs::connect(Share{"nas", "", "", ""}, "hubert", "sekret")), Path{});
    SAME(stage.got("args"), std::string{"mount|smb://nas/|LC_ALL=C\n"});

    stage.put("code-mount", "1");
    stage.put("err-mount", "gio: smb://nas/films/: Failed to mount Windows share: Connection timed out\n");

    SAME(kind_of(loop, tal::fs::connect(Share{"nas", "films", "", ""}, "hubert", "zle")), named(Kind::broken));
}

TRIAL("Share: GVfs: the shares of a server come from gio list's output, sorted, without $; its failure tells why") {
    tal::Loop loop;
    const Stage stage;

    stage.put("out-list", "zdjęcia/\nIPC$/\nfilms/\nmuzyka/\n");
    stage.put("err-list", "(gio:7): GLib-WARNING: noise\n");

    SAME(tal::wait(loop, tal::fs::shares(Share{"nas", "films", "kino", ""})), (std::vector<std::string>{"films", "muzyka", "zdjęcia"}));
    SAME(stage.got("args"), std::string{"mount|smb://nas/|LC_ALL=C\nlist|smb://nas/|LC_ALL=C\n"});

    stage.put("code-list", "1");
    stage.put("err-list", "gio: smb://nas/: Not supported\n");

    SAME(kind_of(loop, tal::fs::shares(Share{"nas", "", "", ""})), named(Kind::unsupported));

    stage.put("code-mount", "1");
    stage.put("err-mount", "gio: smb://nas/: Failed to retrieve share list from server: Permission denied\n");

    SAME(kind_of(loop, tal::fs::shares(Share{"nas", "", "", ""})), named(Kind::denied));

    stage.put("err-mount", "gio: smb://nas/: Location is already mounted\n");

    SAME(kind_of(loop, tal::fs::shares(Share{"nas", "", "", ""})), named(Kind::unsupported));
}

TRIAL("Share: GVfs: without gio, without XDG_RUNTIME_DIR or without a folder for the mount it is unsupported") {
    tal::Loop loop;
    const Stage stage;

    sharing::gio() = Path{"/nonexistent/tal/gio"};

    SAME(kind_of(loop, tal::fs::reach(Share{"nas", "films", "", ""})), named(Kind::unsupported));
    SAME(kind_of(loop, tal::fs::connect(Share{"nas", "films", "", ""}, "hubert", "sekret")), named(Kind::unsupported));
    SAME(kind_of(loop, tal::fs::shares(Share{"nas", "", "", ""})), named(Kind::unsupported));

    sharing::gio() = Path{"gio-of-nobody"};

    SAME(kind_of(loop, tal::fs::reach(Share{"nas", "films", "", ""})), named(Kind::unsupported));

    (void)unsetenv("XDG_RUNTIME_DIR");

    SAME(kind_of(loop, tal::fs::reach(Share{"nas", "films", "", ""})), named(Kind::unsupported));
    SAME(kind_of(loop, tal::fs::connect(Share{"nas", "films", "", ""}, "hubert", "sekret")), named(Kind::unsupported));
    SAME(shown(tal::fs::shared(Path{"/run/gvfs/smb-share:server=nas,share=films"})), std::string{"none"});
}

TRIAL("Share: GVfs: a share gio mounted, though not as a folder, is unsupported; so is an empty XDG_RUNTIME_DIR") {
    tal::Loop loop;
    const Stage stage;

    SAME(kind_of(loop, tal::fs::reach(Share{"nas", "films", "", ""})), named(Kind::unsupported));
    SAME(stage.got("args"), std::string{"mount|smb://nas/films/|LC_ALL=C\n"});

    stage.put("args", "");
    (void)setenv("XDG_RUNTIME_DIR", "", 1);

    SAME(kind_of(loop, tal::fs::reach(Share{"nas", "films", "", ""})), named(Kind::unsupported));
    SAME(kind_of(loop, tal::fs::connect(Share{"nas", "films", "", ""}, "hubert", "sekret")), named(Kind::unsupported));
    SAME(stage.got("args"), std::string{});
}

#endif
