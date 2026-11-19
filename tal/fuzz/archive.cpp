#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "tal/archive.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/port.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

// One input of libFuzzer as a zip, a tar or a tar.gz, by its first byte, on a disk in memory:
// opened, and when it opens, extracted whole. Either may refuse with `Trouble` and nothing else;
// neither may crash, hang, read out of bounds or leak, and nothing may be written outside the
// folder of the extraction.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const bool once = [] {
        tal::defaults().set<tal::fs::Files>(std::make_shared<tal::fs::Memory>());

        return true;
    }();

    (void)once;

    if (size < 1) return 0;

    const char* names[] = {"/a.zip", "/a.tar", "/a.tar.gz"};
    const tal::Path where{names[data[0] % 3]};
    tal::Loop loop;

    try {
        tal::wait(loop, tal::fs::remove(tal::Path{"/out"}, tal::fs::Removing{.trash = false, .deep = true}));
    } catch (const tal::Trouble&) {
    }

    tal::wait(loop, tal::fs::write(where, tal::Bytes(reinterpret_cast<const std::byte*>(data + 1), reinterpret_cast<const std::byte*>(data + size))));

    try {
        const tal::archive::Archive archive = tal::wait(loop, tal::archive::opened(where));

        (void)tal::wait(loop, archive.extracted({tal::archive::Pick{tal::Path{}, tal::Path{"/out"}}}));
    } catch (const tal::Trouble&) {
    }

    for (const tal::fs::Entry& one : tal::wait(loop, tal::fs::list(tal::Path{"/"})))
        if (one.where != tal::Path{"/out"} && one.where != where && one.where.name() != "a.zip" && one.where.name() != "a.tar" && one.where.name() != "a.tar.gz") std::terminate();

    return 0;
}
