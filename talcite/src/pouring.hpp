#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "talcite/fs.hpp"
#include "talcite/promise.hpp"

#include "fscore.hpp"

// The writing of a whole file, shared by copying (`copy.cpp`) and extracting from an archive
// (`archive.cpp`): the counter that reports progress to the promise, the guards that take back what
// an interrupted operation made, and the pouring of a file under its name or beside it.
namespace talcite::fs::core {

// The counter of an operation done in installments: how many bytes or entries out of how many are
// done. Every change goes to the promise of the operation (`Work::progress`), so the progress bar
// asks the promise and nobody has to pass anything upward. A counter without work (a part counted
// apart) only counts.
struct Counted {
    explicit Counted(const Work* given = nullptr) : work(given) {}

    // Sets how much there is in all.
    void total(std::uintmax_t much) {
        whole.store(much);

        told();
    }

    // Sets how much is done.
    void reached(std::uintmax_t much) {
        done.store(much);

        told();
    }

    // Adds to what is done.
    void added(std::uintmax_t much) {
        done.fetch_add(much);

        told();
    }

    // Reports the state to the promise.
    void told() const {
        if (work != nullptr) work->progress(Progress{done.load(), whole.load()});
    }

    std::atomic<std::uintmax_t> done{0};
    std::atomic<std::uintmax_t> whole{0};
    const Work* work = nullptr;
};

// A copy of a file interrupted halfway must not leave behind a file that looks complete. The target
// disappears unless the copy reached the end, and only when the copy created it: a flag the copy
// raises once its exclusive creation succeeded (`made`). A file that someone else put under the
// name in the meantime makes that creation fail, the flag stays down, and their file is not
// removed.
class Sweeping {
public:
    // Watches the target, which goes only when `made` says the copy created it.
    Sweeping(path where, const bool* made) : where_(std::move(where)), made_(made) {}

    // Removes the target, unless `keep` was called or it is not the copy's own.
    ~Sweeping() {
        if (kept_ || !*made_) return;

        std::error_code ignored;
        stood::remove(long_form(where_), ignored);
    }

    Sweeping(const Sweeping&) = delete;
    Sweeping& operator=(const Sweeping&) = delete;
    Sweeping(Sweeping&&) = delete;
    Sweeping& operator=(Sweeping&&) = delete;

    // Marks the target as complete, so that it stays.
    void keep() { kept_ = true; }

private:
    path where_;
    const bool* made_ = nullptr;
    bool kept_ = false;
};

// What a folder copy has made, taken back when the copy does not finish: the files it wrote and the
// folders it created, newest first, a folder only when it is empty by then. Only what the copy made
// goes: a file someone else put into one of its new folders meanwhile stays, and so does the folder
// that holds it. A copy into a folder that stood before (`owning` false) takes nothing back, since
// what arrived is merged with what was there.
class Undoing {
public:
    // Starts with nothing made.
    explicit Undoing(bool owning) : owning_(owning) {}

    // Takes back what was made, unless `keep` was called or the target folder stood before.
    ~Undoing() {
        if (kept_ || !owning_) return;

        for (auto at = made_.rbegin(); at != made_.rend(); ++at) {
            std::error_code ignored;
            stood::remove(long_form(*at), ignored);
        }
    }

    Undoing(const Undoing&) = delete;
    Undoing& operator=(const Undoing&) = delete;
    Undoing(Undoing&&) = delete;
    Undoing& operator=(Undoing&&) = delete;

    // Notes a file written or a folder created by the copy.
    void made(path where) { made_.push_back(std::move(where)); }

    // Marks the copy as complete, so that everything stays.
    void keep() { kept_ = true; }

private:
    std::vector<path> made_;
    bool owning_ = false;
    bool kept_ = false;
};

// How a file is put in place. `durable` waits until the data is on the medium before it says
// "done", as a move does before the source goes. `fresh` says that the target lies in a folder the
// operation has just created itself, where nothing can stand under the name, so the target is not
// asked about.
struct Placing {
    bool durable = false;
    bool fresh = false;
};

// Three kinds of writing to a file: a new one is created exclusively (`Mode::exclusive`) and
// written directly, so a file that someone else put under the name in the meantime is refused, not
// overwritten; a replacement writes beside it and renames, so an interrupted or failed one does not
// touch the old target, and a second hard link to the old file keeps the old content; and a durable
// one (`Placing`) additionally waits until the data is on the medium. A replacement is always made
// durable: without that, a power loss right after the rename could leave an empty file in place of
// the target. A fresh target is not asked about.
//
// `fill` gets the path where the content is to appear and a flag to raise once it has created the
// file there, and returns an open handle to it, or nothing when the task was cancelled halfway;
// then the promise is not settled at all. The modification time, when there is one, is written
// through the same handle before syncing.
template <typename Fill>
bool poured_at(const path& to, Placing placing, std::optional<Instant> when, Fill fill) {
    const bool there = !placing.fresh && kind_at(to) != Kind::none;
    const path into = there ? aside_of(to) : to;

    bool made = false;

    Sweeping sweeping{into, &made};

    {
        const std::shared_ptr<File::Held> yours = fill(into, made);

        if (!yours) return false;

        if (when) stamp_on(*yours, *when);

        if (there || placing.durable) sync_at(*yours);
    }

    if (there && !replaced_at(into, to)) amiss("Cannot replace the file.", to);

    sweeping.keep();

    return true;
}

// Writing in installments through our own buffer, when the content is already at hand or when the
// system cannot copy on its own.
template <typename Pour>
bool poured_at(const path& to, bool durable, Pour pour) {
    const auto fill = [&](const path& into, bool& made) -> std::shared_ptr<File::Held> {
        std::shared_ptr<File::Held> yours = open_at(into, Mode::exclusive);

        made = true;

        if (!pour(*yours)) return nullptr;

        return yours;
    };

    return poured_at(to, Placing{.durable = durable}, std::nullopt, fill);
}

}
