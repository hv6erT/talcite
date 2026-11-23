#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

#include "tal/bytes.hpp"
#include "tal/clock.hpp"
#include "tal/docs.hpp"
#include "tal/executor.hpp"
#include "tal/meta.hpp"
#include "tal/pixels.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "quieting.hpp"
#include "rendering.hpp"
#include "tal/testing.hpp"

// Documents and pages rendered by another process: the frames of the protocol, the adapters on a
// program that speaks it, here a toy engine run as a script (and, where only a real process shows
// it, as this binary pretending), and the chains of the ports. The toy keeps to the protocol
// unless told to break it in one way or another.

namespace {

using namespace std::chrono_literals;

using tal::Bytes;
using tal::Command;
using tal::Kind;
using tal::Task;
using tal::docs::Browsing;
using tal::docs::Page;
using tal::docs::Region;
using tal::media::Pixels;
using tal::media::Size;
using tal::rendering::Frame;
using tal::rendering::Sort;

// The kind and the message of the trouble `call` throws, or "none".
template <typename Call>
std::string refusal(Call call) {
    try {
        call();
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())} + ": " + why.what();
    }

    return "none";
}

// The kind of the trouble `call` throws, or nothing.
template <typename Call>
std::optional<Kind> kind_of(Call call) {
    try {
        call();
    } catch (const tal::Trouble& why) {
        return why.kind();
    }

    return std::nullopt;
}

// The bytes of the text `text`.
Bytes bytes_of(std::string_view text) {
    Bytes made;

    for (const char one : text) made.push_back(static_cast<std::byte>(one));

    return made;
}

// A frame of `sort` with `id` and `payload`.
Frame frame_of(Sort sort, std::uint32_t id, Bytes payload) {
    Frame made;

    made.sort = sort;
    made.id = id;
    made.payload = std::move(payload);

    return made;
}

// A frame of `sort` with `id` and nothing in it.
Frame frame_of(Sort sort, std::uint32_t id) { return frame_of(sort, id, Bytes()); }

// The way the toy engine reads and writes frames: a script's streams, or the standard streams of
// a pretending process.
class Wire {
public:
    Wire() = default;
    virtual ~Wire() = default;

    Wire(const Wire&) = delete;
    Wire& operator=(const Wire&) = delete;

    // The next frame, or nothing at the end.
    virtual Task<std::optional<Frame>> next() = 0;

    // Writes `bytes` as they are.
    virtual Task<void> sent(Bytes bytes) = 0;
};

// The wire of a script: its input and output streams.
class Scripted final : public Wire {
public:
    // The streams of `running`.
    explicit Scripted(tal::Running& running) : running_(running) {}

    // The next frame from the script's input, or nothing when it ends.
    Task<std::optional<Frame>> next() override {
        for (;;) {
            std::optional<Frame> made = frames_.next();

            if (made) co_return made;

            const Bytes got = co_await running_.input.read(std::size_t{64} << 10);

            if (got.empty()) co_return std::nullopt;

            frames_.add(got);
        }
    }

    // Writes to the script's output.
    Task<void> sent(Bytes bytes) override { co_await running_.output.write(std::move(bytes)); }

private:
    tal::Running& running_;
    tal::rendering::Unframing frames_;
};

// The wire of a pretending process: its standard streams, read frame by frame.
class Standard final : public Wire {
public:
    // The next frame from the standard input, or nothing when it ends.
    Task<std::optional<Frame>> next() override {
        Bytes head(4);

        if (std::fread(head.data(), 1, 4, stdin) != 4) co_return std::nullopt;

        const std::size_t length = std::to_integer<std::size_t>(head[0]) | (std::to_integer<std::size_t>(head[1]) << 8) |
                                   (std::to_integer<std::size_t>(head[2]) << 16) | (std::to_integer<std::size_t>(head[3]) << 24);
        Bytes rest(length);

        if (std::fread(rest.data(), 1, length, stdin) != length) co_return std::nullopt;

        tal::rendering::Unframing frames;

        frames.add(head);
        frames.add(rest);
        co_return frames.next();
    }

    // Writes to the standard output and flushes it.
    Task<void> sent(Bytes bytes) override {
        (void)std::fwrite(bytes.data(), 1, bytes.size(), stdout);
        (void)std::fflush(stdout);
        co_return;
    }
};

// What the toy is told to do, from the words of its command: "silent" (no hello), "die-first"
// (ends before its hello), "old" (greets in version 1), "early" (an answer before its hello, which
// carries what a hello would), "unavailable=<sentence>" (says its engine is not there, before any
// hello, and ends), "flood" (twenty refusals after opening a view), "stranger" (a change of a view
// it never had, after opening one), "long" (a change with a title over 64 KiB), "flag" (a change
// with a flag of 2), "reason" (a refusal of no reason), "numbered" (a change with an id), "lose" (a
// click loses the view),
// "pages-only" (no documents), "scripts" and "network" (it can allow them); "open=<how>" for what
// an opening does: many (claims four billion pages), flat (a page of no width), greedy (asks for
// more than a block); "render=<how>" for what a render does: hang, die, wide and tall (a picture
// a pixel wider or higher than asked), short
// (too few bytes), garbage (a frame of no sort), orphan (an answer to no question), huge (a frame
// too long), fail (a trouble), oddity (a trouble of no kind), reversed (answers two renders in the
// reverse order), hog (takes 512 MiB); "password=<word>" (its documents open only with that
// password). As a real process alone: "deaf" (after its first opening it
// listens no more, not even for its input closing) and "pidfile=<path>" (writes its number there).
struct Told {
    bool silent = false;
    bool die_first = false;
    bool old = false;
    bool documents = true;
    bool scripts = false;
    bool network = false;
    bool early = false;
    bool deaf = false;
    bool flood = false;
    bool stranger = false;
    bool long_title = false;
    bool bad_flag = false;
    bool bad_reason = false;
    bool numbered = false;
    bool lose = false;
    std::string unavailable;
    std::string pidfile;
    std::string password;
    std::string render = "answer";
    std::string open = "answer";

    // The toy as `words` tell it.
    explicit Told(std::span<const std::string> words) {
        for (const std::string& one : words) {
            if (one == "silent") silent = true;
            if (one == "die-first") die_first = true;
            if (one == "old") old = true;
            if (one == "pages-only") documents = false;
            if (one == "scripts") scripts = true;
            if (one == "network") network = true;
            if (one == "early") early = true;
            if (one == "deaf") deaf = true;
            if (one == "flood") flood = true;
            if (one == "stranger") stranger = true;
            if (one == "long") long_title = true;
            if (one == "flag") bad_flag = true;
            if (one == "reason") bad_reason = true;
            if (one == "numbered") numbered = true;
            if (one == "lose") lose = true;
            if (one.starts_with("unavailable=")) unavailable = one.substr(12);
            if (one.starts_with("pidfile=")) pidfile = one.substr(8);
            if (one.starts_with("password=")) password = one.substr(9);
            if (one.starts_with("render=")) render = one.substr(7);
            if (one.starts_with("open=")) open = one.substr(5);
        }
    }
};

// How many toys live now, of every bench.
std::atomic<int> toys{0};

// A toy living: counted while its coroutine lives, whether it ends or is killed.
struct Living {
    // Counts one more.
    Living() { ++toys; }

    // Counts one less.
    ~Living() { --toys; }

    Living(const Living&) = delete;
    Living& operator=(const Living&) = delete;
};

// The payload of a failed answer for `kind` and `said`.
Bytes failed_payload(Kind kind, std::string_view said) { return tal::rendering::Writing().u8(static_cast<std::uint8_t>(kind)).text(said).done(); }

// The picture of a render of `page` at `size` of `region`: red the page, green the left edge and
// blue the width of the region, both in 255ths.
Bytes rendered_payload(std::uint32_t page, Size size, double left, double width) {
    tal::rendering::Writing made;

    made.u32(size.width).u32(size.height);

    const auto green = static_cast<std::uint8_t>(left * 255 + 0.5);
    const auto blue = static_cast<std::uint8_t>(width * 255 + 0.5);

    for (std::uint64_t at = 0; at < std::uint64_t{size.width} * size.height; ++at) made.u8(static_cast<std::uint8_t>(page)).u8(green).u8(blue).u8(255);

    return made.done();
}

// The payload of a change of the view `view`: its title `title` and address `address`, loaded,
// with a way back.
Bytes changed_payload(std::uint32_t view, std::string title, std::string address) {
    tal::rendering::Changed made;

    made.view = view;
    made.title = std::move(title);
    made.address = std::move(address);
    made.back = true;

    return tal::rendering::changed_with(made);
}

// The pages of a toy document whose bytes say "pages:N": N pages, 100 wide and 100 + 10 * i high.
Bytes pages_payload(const Bytes& content) {
    const std::string text{reinterpret_cast<const char*>(content.data()), content.size()};
    const int count = text.starts_with("pages:") ? std::atoi(text.c_str() + 6) : 1;
    tal::rendering::Writing made;

    made.u32(static_cast<std::uint32_t>(count));

    for (int one = 0; one < count; ++one) made.f64(100).f64(100 + 10.0 * one);

    return made.done();
}

// The toy engine on `wire`, doing what `told` says, its reads of documents asked five bytes at a
// time. Returns its exit code.
Task<int> toy(Wire& wire, Told told) {
    const Living living;

    if (told.die_first) co_return 3;

    tal::rendering::Hello hello;

    if (!told.unavailable.empty()) {
        co_await wire.sent(tal::rendering::framed(frame_of(Sort::failed, 0, failed_payload(Kind::unsupported, told.unavailable))));
        co_return 6;
    }

    hello.version = told.old ? 1 : tal::rendering::version;
    hello.engine = "toy";
    hello.abilities = static_cast<std::uint8_t>((told.documents ? tal::rendering::documents : 0) | tal::rendering::pages |
                                                (told.scripts ? tal::rendering::scripts : 0) | (told.network ? tal::rendering::network : 0));

    if (told.early) co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, 1, tal::rendering::greeted(hello))));

    if (!told.silent) co_await wire.sent(tal::rendering::framed(frame_of(Sort::hello, 0, tal::rendering::greeted(hello))));

    std::uint32_t reads = 0;
    std::vector<Frame> held;
    Size view{0, 0};
    std::string address;
    bool scripts = false;
    bool network = false;

    for (;;) {
        std::optional<Frame> got = co_await wire.next();

        if (!got) co_return 0;

        tal::rendering::Reading from{got->payload};

        if (got->sort == Sort::open) {
            const std::uint32_t document = from.u32();
            const std::uint64_t size = from.u64();
            const std::string password = from.text();
            Bytes content;
            std::optional<Bytes> refused;

            if (password != told.password) {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::failed, got->id, tal::rendering::failed_with(tal::rendering::locked(password.empty())))));
                continue;
            }

            if (told.open == "greedy") {
                co_await wire.sent(tal::rendering::framed(
                    frame_of(Sort::read, ++reads, tal::rendering::Writing().u32(document).u64(0).u32(tal::rendering::widest_block + 1).done())));

                std::optional<Frame> block = co_await wire.next();

                if (!block) co_return 0;

                co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id, pages_payload(block->payload))));
                continue;
            }

            if (told.open == "many") {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id, tal::rendering::Writing().u32(0xFFFFFFFF).f64(1).f64(1).done())));
                continue;
            }

            if (told.open == "flat") {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id, tal::rendering::Writing().u32(1).f64(0).f64(10).done())));
                continue;
            }

            while (content.size() < size && !refused) {
                const auto much = static_cast<std::uint32_t>(std::min<std::uint64_t>(5, size - content.size()));

                co_await wire.sent(tal::rendering::framed(frame_of(Sort::read, ++reads, tal::rendering::Writing().u32(document).u64(content.size()).u32(much).done())));

                std::optional<Frame> block = co_await wire.next();

                if (!block) co_return 0;

                if (block->sort == Sort::unread) {
                    refused = block->payload;
                } else {
                    content.insert(content.end(), block->payload.begin(), block->payload.end());
                }
            }

            if (refused) {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::failed, got->id, *refused)));
            } else {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id, pages_payload(content))));
            }

            while (told.deaf) std::this_thread::sleep_for(std::chrono::seconds{1});
        } else if (got->sort == Sort::render) {
            (void)from.u32();

            const std::uint32_t page = from.u32();
            const std::uint32_t wide = from.u32();
            const std::uint32_t high = from.u32();
            const double left = from.f64();

            (void)from.f64();

            const double width = from.f64();
            const Size size{wide, high};

            if (told.render == "hang") continue;

            if (told.render == "die") co_return 4;

            if (told.render == "hog") {
                const std::size_t much = std::size_t{512} << 20;
                auto* taken = new (std::nothrow) unsigned char[much];

                if (taken == nullptr) co_return 5;

                for (std::size_t at = 0; at < much; at += 4096) taken[at] = 1;

                delete[] taken;
            }

            if (told.render == "wide" || told.render == "tall") {
                Size lied = size;

                if (told.render == "wide") {
                    ++lied.width;
                } else {
                    ++lied.height;
                }

                co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id, rendered_payload(page, lied, left, width))));
            } else if (told.render == "garbage") {
                co_await wire.sent(tal::rendering::framed(frame_of(static_cast<Sort>(99), got->id)));
            } else if (told.render == "orphan") {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id + 1000, rendered_payload(page, size, left, width))));
            } else if (told.render == "huge") {
                co_await wire.sent(tal::rendering::Writing().u32(0xFFFFFFF0).done());
            } else if (told.render == "short") {
                Bytes cut = rendered_payload(page, size, left, width);

                cut.pop_back();
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id, cut)));
            } else if (told.render == "oddity") {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::failed, got->id, tal::rendering::Writing().u8(200).text("odd").done())));
            } else if (told.render == "fail") {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::failed, got->id, failed_payload(Kind::unsupported, "No such feature."))));
            } else if (told.render == "reversed") {
                held.push_back(frame_of(Sort::done, got->id, rendered_payload(page, size, left, width)));

                if (held.size() == 2) {
                    co_await wire.sent(tal::rendering::framed(held[1]));
                    co_await wire.sent(tal::rendering::framed(held[0]));
                    held.clear();
                }
            } else {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id, rendered_payload(page, size, left, width))));
            }
        } else if (got->sort == Sort::navigate) {
            const std::uint32_t number = from.u32();

            address = from.text();
            scripts = from.u8() != 0;
            network = from.u8() != 0;
            view.width = from.u32();
            view.height = from.u32();
            co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id)));
            co_await wire.sent(tal::rendering::framed(frame_of(Sort::changed, 0, changed_payload(number, address, address))));

            for (int one = 0; told.flood && one < 20; ++one)
                co_await wire.sent(tal::rendering::framed(
                    frame_of(Sort::refused, 0, tal::rendering::refused_with(tal::rendering::Refused{number, 1, "https://example.test/" + std::to_string(one)}))));

            if (told.stranger) co_await wire.sent(tal::rendering::framed(frame_of(Sort::changed, 0, changed_payload(number + 1000, "who", "?"))));

            if (told.bad_flag) {
                Bytes flagged = changed_payload(number, "flag", address);

                flagged.back() = std::byte{2};
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::changed, 0, std::move(flagged))));
            }

            if (told.bad_reason)
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::refused, 0, tal::rendering::refused_with(tal::rendering::Refused{number, 9, "x"}))));

            if (told.numbered) co_await wire.sent(tal::rendering::framed(frame_of(Sort::changed, 7, changed_payload(number, "numbered", address))));

            if (told.long_title) co_await wire.sent(tal::rendering::framed(frame_of(Sort::changed, 0, changed_payload(number, std::string((64 << 10) + 1, 'x'), "?"))));
        } else if (got->sort == Sort::go || got->sort == Sort::click || got->sort == Sort::wheel || got->sort == Sort::key || got->sort == Sort::type ||
                   got->sort == Sort::zoom) {
            const std::uint32_t number = from.u32();
            std::string heard;

            if (got->sort == Sort::go) {
                const std::uint8_t how = from.u8();

                heard = "go " + std::to_string(how);

                if (how == 0) {
                    const std::string to = from.text();

                    if (to.starts_with("http") && !network) {
                        co_await wire.sent(tal::rendering::framed(frame_of(Sort::failed, got->id, failed_payload(Kind::refused, "The view may not go there."))));
                        continue;
                    }

                    address = to;
                    heard += " " + to;
                }
            } else if (got->sort == Sort::click || got->sort == Sort::wheel) {
                const double x = from.f64();
                const double y = from.f64();

                heard = std::string{got->sort == Sort::click ? "click " : "wheel "} + std::to_string(x) + " " + std::to_string(y);
            } else if (got->sort == Sort::key) {
                const std::uint8_t name = from.u8();
                const std::uint32_t letter = from.u32();
                const std::uint8_t mods = from.u8();

                heard = "key " + std::to_string(name) + " " + std::to_string(letter) + " " + std::to_string(mods);
            } else if (got->sort == Sort::type) {
                heard = "type " + from.text();
            } else {
                heard = "zoom " + std::to_string(from.f64());
            }

            co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id)));

            if (told.lose && got->sort == Sort::click) {
                co_await wire.sent(tal::rendering::framed(frame_of(Sort::lost, 0, tal::rendering::Writing().u32(number).done())));
                continue;
            }

            co_await wire.sent(tal::rendering::framed(frame_of(Sort::changed, 0, changed_payload(number, heard, address))));
        } else if (got->sort == Sort::shot) {
            tal::rendering::Writing made;
            const std::string title = std::string{"scripts="} + (scripts ? "1" : "0") + " network=" + (network ? "1" : "0");

            made.text(title).text(address).u32(view.width).u32(view.height);

            for (std::uint64_t at = 0; at < std::uint64_t{view.width} * view.height; ++at) made.u8(255).u8(255).u8(255).u8(255);

            co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id, made.done())));
        } else if (got->sort == Sort::resize) {
            (void)from.u32();
            view.width = from.u32();
            view.height = from.u32();
            co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id)));
        } else {
            co_await wire.sent(tal::rendering::framed(frame_of(Sort::done, got->id)));
        }
    }
}

// The toy as a script: what it is told comes from its command's arguments.
Task<int> scripted_toy(tal::Running& running) {
    Scripted wire{running};
    const Told told(running.command.arguments);

    co_return co_await toy(wire, told);
}

// The number of this process.
long long own_number() {
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

// Whether the process `number` still runs.
bool running(long long number) {
#ifdef _WIN32
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(number));

    if (process == nullptr) return false;

    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;

    CloseHandle(process);

    return alive;
#else
    return ::kill(static_cast<pid_t>(number), 0) == 0;
#endif
}

// A toy that may outlive a failed trial, known by its number.
struct Stray {
    long long number = 0;

    Stray() = default;

    // Kills the process numbered `number` when it still runs, so a failed trial leaves no toy
    // behind.
    ~Stray() {
        if (number <= 0 || !running(number)) return;
#ifdef _WIN32
        const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(number));

        if (process == nullptr) return;

        TerminateProcess(process, 1);
        CloseHandle(process);
#else
        ::kill(static_cast<pid_t>(number), SIGKILL);
#endif
    }

    Stray(const Stray&) = delete;
    Stray& operator=(const Stray&) = delete;
};

// The toy as this binary pretending, on its standard streams; its number written where it is
// told to.
int pretended_toy(std::span<const std::string> words) {
    tal::Loop loop;
    Standard wire;
    const Told told(words);

    if (!told.pidfile.empty()) std::ofstream{told.pidfile} << own_number();

    return tal::wait(loop, toy(wire, told));
}

const tal::testing::Pretending pretending_toy{"toy-renderer", &pretended_toy};

// The toy as a program of scripted processes and the adapters on it, asked from a loop with a
// manual clock. The scripts run on the pool of io, as the engine's own work does, so that what the
// engine started a moment before the bench goes never reaches a loop that is gone.
struct Bench {
    std::shared_ptr<tal::ManualClock> clock = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{clock}};
    std::shared_ptr<tal::Scripted> processes = std::make_shared<tal::Scripted>(tal::io());
    tal::docs::Limits limits;

    // The bench, its toy known as "toy".
    Bench() {
        processes->add("toy", [](tal::Running& running) { return scripted_toy(running); });
    }

    // The renderer on the toy told `words`, taking every document.
    std::shared_ptr<tal::docs::Renderer> renderer(std::vector<std::string> words = {}) {
        Command program;

        program.program = tal::Path{"toy"};
        program.arguments = std::move(words);

        return tal::docs::renderer(program, [](std::span<const std::byte>) { return true; }, limits, tal::Port<tal::Processes>{processes});
    }

    // The browser on the toy told `words`.
    std::shared_ptr<tal::docs::Browser> browser(std::vector<std::string> words = {}) {
        Command program;

        program.program = tal::Path{"toy"};
        program.arguments = std::move(words);

        return tal::docs::browser(program, limits, tal::Port<tal::Processes>{processes});
    }

    // The document `text` opened by `renderer` with `password`.
    std::shared_ptr<tal::docs::Document> opened(const std::shared_ptr<tal::docs::Renderer>& renderer, std::string_view text = "pages:3",
                                                std::string password = {}) {
        return tal::wait(loop, renderer->opened(std::make_shared<tal::meta::Held>(bytes_of(text)), password));
    }

    // Page `page` of `document` at `size` of `region`.
    Pixels rendered(const std::shared_ptr<tal::docs::Document>& document, std::size_t page, Size size = {2, 1}, Region region = {}) {
        return tal::wait(loop, document->rendered(page, size, region));
    }

    // How many processes were started.
    std::size_t started() const { return processes->started().size(); }
};

// The bands of the first pixel of `pixels`.
std::vector<int> first_pixel(const Pixels& pixels) {
    std::vector<int> made;

    for (std::size_t at = 0; at < 4; ++at) made.push_back(std::to_integer<int>(pixels.stills().front().rgba[at]));

    return made;
}

// A browser that fails with `kind` whatever it is asked.
class Failing final : public tal::docs::Browser {
public:
    // Fails with `kind`.
    explicit Failing(Kind kind) : kind_(kind) {}

    std::string_view name() const override { return "failing"; }

    Task<std::shared_ptr<tal::docs::Browsed>> opened(std::string, Browsing, Size) override { throw tal::Trouble{kind_, "It fails."}; }

private:
    Kind kind_;
};

// A reader whose every read fails as missing.
class Gone final : public tal::meta::Reader {
public:
    // Ten bytes, it says.
    Task<std::uint64_t> size() override { co_return 10; }

    // Fails.
    Task<Bytes> read(std::uint64_t, std::size_t) override { throw tal::Trouble{Kind::missing, "The file went away."}; }
};

}

// A frame is its length, sort, number and payload; it comes whole in pieces of any size and
// several in one piece. A length shorter than the sort and the number, or over the most allowed,
// breaks the protocol; so does a payload read past its end or not to it, and a number that is not
// finite. A text with broken UTF-8 comes mended.
TRIAL("Rendering: frames whole and in pieces, their shapes checked") {
    const Bytes one = tal::rendering::framed(frame_of(Sort::render, 7, bytes_of("abc")));

    SAME(one.size(), std::size_t{12});
    SAME(std::to_integer<int>(one[0]), 8);
    SAME(std::to_integer<int>(one[4]), 11);
    SAME(std::to_integer<int>(one[5]), 7);

    tal::rendering::Unframing frames;

    for (const std::byte each : one) {
        OUGHT(!frames.next().has_value());
        frames.add(std::span<const std::byte>{&each, 1});
    }

    const std::optional<Frame> got = frames.next();

    OUGHT(got.has_value() && got->sort == Sort::render && got->id == 7 && got->payload == bytes_of("abc"));
    OUGHT(frames.empty());

    Bytes two = tal::rendering::framed(frame_of(Sort::done, 1));
    const Bytes three = tal::rendering::framed(frame_of(Sort::failed, 2, bytes_of("x")));

    two.insert(two.end(), three.begin(), three.end());
    frames.add(two);
    SAME(frames.next()->id, std::uint32_t{1});
    OUGHT(frames.next()->payload == bytes_of("x"));
    OUGHT(!frames.next().has_value());

    tal::rendering::Unframing short_one;

    short_one.add(tal::rendering::Writing().u32(4).u8(1).done());
    SAME(kind_of([&] { (void)short_one.next(); }), std::optional<Kind>{Kind::malformed});

    tal::rendering::Unframing small{64};

    small.add(tal::rendering::Writing().u32(61).done());
    SAME(kind_of([&] { (void)small.next(); }), std::optional<Kind>{Kind::malformed});

    tal::rendering::Unframing fits{64};

    fits.add(tal::rendering::Writing().u32(60).done());
    OUGHT(!fits.next().has_value());

    const Bytes payload = tal::rendering::Writing().u8(9).u32(70000).u64(std::uint64_t{1} << 40).f64(-2.5).text("zażółć").done();
    tal::rendering::Reading from{payload};

    SAME(from.u8(), std::uint8_t{9});
    SAME(from.u32(), std::uint32_t{70000});
    SAME(from.u64(), std::uint64_t{1} << 40);
    SAME(from.f64(), -2.5);
    SAME(from.text(), std::string{"zażółć"});
    from.ended();
    SAME(kind_of([&] { (void)from.u8(); }), std::optional<Kind>{Kind::malformed});

    tal::rendering::Reading left{payload};

    (void)left.u8();
    SAME(kind_of([&] { left.ended(); }), std::optional<Kind>{Kind::malformed});

    const Bytes nan = tal::rendering::Writing().u64(0x7FF8000000000000).done();
    tal::rendering::Reading odd{nan};

    SAME(kind_of([&] { (void)odd.f64(); }), std::optional<Kind>{Kind::malformed});

    const Bytes long_text = tal::rendering::Writing().u32(100).u8('a').done();
    tal::rendering::Reading cut{long_text};

    SAME(kind_of([&] { (void)cut.text(); }), std::optional<Kind>{Kind::malformed});

    Bytes broken = tal::rendering::Writing().u32(2).done();

    broken.push_back(std::byte{'a'});
    broken.push_back(std::byte{0xFF});

    tal::rendering::Reading mended{broken};

    SAME(mended.text(), std::string{"a\xEF\xBF\xBD"});

    tal::rendering::Hello hello;

    hello.version = 1;
    hello.engine = "toy";
    hello.abilities = 5;

    const tal::rendering::Hello back = tal::rendering::greeting(tal::rendering::greeted(hello));

    OUGHT(back.version == 1 && back.engine == "toy" && back.abilities == 5);
    SAME(kind_of([&] { (void)tal::rendering::greeting(tal::rendering::Writing().u32(1).done()); }), std::optional<Kind>{Kind::malformed});
}

// A document goes to the process only as the blocks the process asks for (five bytes at a time,
// here), which say how many pages it has; a page comes at the size asked, of the region asked; a
// document let go is closed in the process, whose answer to that is taken, and the other document
// renders on. The process is started once for all of it.
TRIAL("Rendering: a document read by blocks, its pages rendered by region, closed") {
    Bench bench;
    const auto renderer = bench.renderer();

    SAME(renderer->name(), std::string_view{"toy"});

    auto document = bench.opened(renderer, "pages:3");

    OUGHT(document->pages() == (std::vector<Page>{{100, 100}, {100, 110}, {100, 120}}));

    const Pixels whole = bench.rendered(document, 2, Size{4, 3});

    OUGHT(whole.size() == (Size{4, 3}));
    OUGHT(first_pixel(whole) == (std::vector<int>{2, 0, 255, 255}));
    OUGHT(first_pixel(bench.rendered(document, 0, Size{1, 1}, Region{0.5, 0.25, 0.5, 0.5})) == (std::vector<int>{0, 128, 128, 255}));

    auto other = bench.opened(renderer, "pages:1");

    SAME(other->pages().size(), std::size_t{1});
    document.reset();
    QUIETED(bench.loop);
    OUGHT(first_pixel(bench.rendered(other, 0)) == (std::vector<int>{0, 0, 255, 255}));
    other.reset();
    QUIETED(bench.loop);
    SAME(bench.started(), std::size_t{1});
}

// What the process answers as a trouble passes as it is; a block the program cannot read fails the
// opening with the trouble of the read; a page, a size or a region out of range is the caller's
// mistake, and the process is not asked.
TRIAL("Rendering: troubles answered, blocks unread, questions out of range") {
    Bench bench;
    const auto failing = bench.renderer({"render=fail"});
    const auto document = bench.opened(failing);

    SAME(refusal([&] { (void)bench.rendered(document, 0); }), std::string{"unsupported: No such feature."});
    SAME(refusal([&] { (void)tal::wait(bench.loop, failing->opened(std::make_shared<Gone>(), {})); }), std::string{"missing: The file went away."});
    SAME(refusal([&] { (void)bench.rendered(document, 3); }), std::string{"other: No such page."});
    SAME(refusal([&] { (void)bench.rendered(document, 0, Size{0, 4}); }), std::string{"other: A size of no pixels or over the ceiling."});
    SAME(refusal([&] { (void)bench.rendered(document, 0, Size{4000, 2001}); }), std::string{"other: A size of no pixels or over the ceiling."});
    SAME(refusal([&] { (void)bench.rendered(document, 0, Size{1, 1}, Region{0.5, 0, 0.6, 1}); }), std::string{"other: A region not within the page."});
    SAME(refusal([&] { (void)bench.rendered(document, 0, Size{1, 1}, Region{0, 0, 0, 1}); }), std::string{"other: A region not within the page."});
    SAME(refusal([&] { (void)bench.rendered(document, 0, Size{1, 1}, Region{-0.1, 0, 0.5, 1}); }), std::string{"other: A region not within the page."});
    SAME(bench.started(), std::size_t{1});
}

// A process that breaks the protocol is killed: a picture of another size than asked or of too few
// bytes, a frame of no sort, an answer to no question, a frame longer than any may be, a trouble of
// no kind. What waited fails as malformed, the process is gone, the document is lost with it (also
// once the next opening has started another process), and the next opening starts it again. An
// opening that claims more pages than it sent or a page of no area, or that asks for more than a
// block, breaks it too; so does an answer before the hello.
TRIAL("Rendering: a process that breaks the protocol is killed, the next opening starts anew") {
    for (const std::string how : {"render=wide", "render=tall", "render=short", "render=garbage", "render=orphan", "render=huge", "render=oddity"}) {
        Bench bench;
        const auto renderer = bench.renderer({how});
        const auto document = bench.opened(renderer);

        SAME(kind_of([&] { (void)bench.rendered(document, 0); }), std::optional<Kind>{Kind::malformed});
        UNTIL(bench.loop, toys.load() == 0);
        SAME(refusal([&] { (void)bench.rendered(document, 0); }), std::string{"broken: The renderer toy ended: what was open in it was lost."});

        const auto again = bench.opened(renderer);

        SAME(again->pages().size(), std::size_t{3});
        SAME(bench.started(), std::size_t{2});
        SAME(refusal([&] { (void)bench.rendered(document, 0); }), std::string{"broken: The renderer toy ended: what was open in it was lost."});
    }

    for (const std::string how : {"open=many", "open=flat", "open=greedy"}) {
        Bench bench;
        const auto renderer = bench.renderer({how});

        SAME(kind_of([&] { (void)bench.opened(renderer); }), std::optional<Kind>{Kind::malformed});
        SAME(bench.started(), std::size_t{1});
    }

    Bench early;

    SAME(kind_of([&] { (void)early.opened(early.renderer({"early"})); }), std::optional<Kind>{Kind::unsupported});
}

// A question unanswered within its limit is overdue and the process killed; a process that ends
// while a question waits breaks it. Either way the next opening starts the process anew.
TRIAL("Rendering: a hang is overdue, a death breaks, and the process starts anew") {
    Bench bench;

    bench.limits.answer = 2000ms;

    const auto hanging = bench.renderer({"render=hang"});
    const auto document = bench.opened(hanging);
    tal::Scope scope{bench.loop};
    std::optional<std::string> said;

    scope.run([](std::shared_ptr<tal::docs::Document> one, std::optional<std::string>& into) -> Task<void> {
        const Size dot{1, 1};
        const Region whole;

        try {
            (void)co_await one->rendered(0, dot, whole);
            into = "none";
        } catch (const tal::Trouble& why) {
            into = std::string{tal::spoken(why.kind())} + ": " + why.what();
        }
    }(document, said));

    QUIETED(bench.loop);
    bench.clock->advance(1999ms);
    QUIETED(bench.loop);
    OUGHT(!said.has_value());
    bench.clock->advance(1ms);
    UNTIL(bench.loop, said.has_value());
    SAME(*said, std::string{"overdue: The renderer toy did not answer in time (render)."});
    SAME(bench.opened(hanging)->pages().size(), std::size_t{3});
    SAME(bench.started(), std::size_t{2});

    const auto dying = bench.renderer({"render=die"});
    const auto doomed = bench.opened(dying);

    SAME(kind_of([&] { (void)bench.rendered(doomed, 0); }), std::optional<Kind>{Kind::broken});
    SAME(bench.opened(dying)->pages().size(), std::size_t{3});
    SAME(bench.started(), std::size_t{4});
}

// An engine that cannot start, ends or keeps silent before its hello, or greets in another version
// is not there: unsupported, its process gone, and not started again while the adapter lives. One
// that renders no documents refuses to open one.
TRIAL("Rendering: an engine that does not greet is not there, and not started again") {
    for (const std::string how : {"die-first", "silent", "old"}) {
        Bench bench;

        bench.limits.greeting = 1000ms;

        const auto renderer = bench.renderer({how});
        tal::Scope scope{bench.loop};
        std::optional<std::optional<Kind>> first;

        scope.run([](std::shared_ptr<tal::docs::Renderer> one, std::optional<std::optional<Kind>>& into) -> Task<void> {
            try {
                (void)co_await one->opened(std::make_shared<tal::meta::Held>(bytes_of("pages:1")), {});
                into = std::optional<Kind>{};
            } catch (const tal::Trouble& why) {
                into = why.kind();
            }
        }(renderer, first));

        UNTIL(bench.loop, bench.started() == 1);
        QUIETED(bench.loop);

        if (how == "silent") bench.clock->advance(1000ms);

        UNTIL(bench.loop, first.has_value());
        SAME(*first, std::optional<Kind>{Kind::unsupported});
        UNTIL(bench.loop, toys.load() == 0);
        SAME(kind_of([&] { (void)bench.opened(renderer); }), std::optional<Kind>{Kind::unsupported});
        SAME(bench.started(), std::size_t{1});
    }

    Bench missing;
    Command nothing;

    nothing.program = tal::Path{"nothing"};

    const auto absent = tal::docs::renderer(nothing, [](std::span<const std::byte>) { return true; }, {}, tal::Port<tal::Processes>{missing.processes});

    SAME(kind_of([&] { (void)missing.opened(absent); }), std::optional<Kind>{Kind::unsupported});

    Bench pages;

    SAME(refusal([&] { (void)pages.opened(pages.renderer({"pages-only"})); }), std::string{"unsupported: The renderer toy renders no documents."});
}

// Two renders asked at once are answered in the reverse order, and each answer reaches its own
// asker.
TRIAL("Rendering: answers out of order reach their askers") {
    Bench bench;
    const auto renderer = bench.renderer({"render=reversed"});
    const auto document = bench.opened(renderer);
    tal::Scope scope{bench.loop};
    std::vector<std::optional<Pixels>> got(2);

    for (std::size_t one = 0; one < 2; ++one)
        scope.run([](std::shared_ptr<tal::docs::Document> from, std::size_t page, std::optional<Pixels>& into) -> Task<void> {
            const Size dot{1, 1};
            const Region whole;

            into = co_await from->rendered(page, dot, whole);
        }(document, one, got[one]));

    UNTIL(bench.loop, got[0].has_value() && got[1].has_value());
    SAME(first_pixel(*got[0])[0], 0);
    SAME(first_pixel(*got[1])[0], 1);
}

// A page opens with scripts and the network as the program asks, both off unless asked; its shot
// comes at its size, which a resize changes. An engine that cannot allow scripts or the network
// refuses a page that asks for them.
TRIAL("Browsing: a page with scripts and network as asked, shot and resized") {
    Bench bench;
    const auto browser = bench.browser({"scripts", "network"});
    const auto plain = tal::wait(bench.loop, browser->opened("file:///a.html", Browsing{}, Size{3, 2}));
    const tal::docs::Shot shot = tal::wait(bench.loop, plain->shot());

    SAME(shot.title, std::string{"scripts=0 network=0"});
    SAME(shot.address, std::string{"file:///a.html"});
    OUGHT(shot.pixels.size() == (Size{3, 2}));

    const auto live = tal::wait(bench.loop, browser->opened("https://example.org/", Browsing{true, true}, Size{3, 2}));

    SAME(tal::wait(bench.loop, live->shot()).title, std::string{"scripts=1 network=1"});
    tal::wait(bench.loop, live->resized(Size{5, 4}));
    OUGHT(tal::wait(bench.loop, live->shot()).pixels.size() == (Size{5, 4}));
    SAME(kind_of([&] { tal::wait(bench.loop, live->resized(Size{0, 4})); }), std::optional<Kind>{Kind::other});

    const auto careful = bench.browser();

    SAME(refusal([&] { (void)tal::wait(bench.loop, careful->opened("a", Browsing{true, false}, Size{1, 1})); }),
         std::string{"unsupported: The renderer toy runs no scripts."});
    SAME(refusal([&] { (void)tal::wait(bench.loop, careful->opened("a", Browsing{false, true}, Size{1, 1})); }),
         std::string{"unsupported: The renderer toy has no network."});
    SAME(tal::wait(bench.loop, tal::wait(bench.loop, careful->opened("a", Browsing{}, Size{1, 1}))->shot()).title, std::string{"scripts=0 network=0"});
}

// Version 2: a view goes where it is told, back and forward, takes clicks, the wheel, keys
// (letters as typed text), text and zoom, each answered once the process has taken it; what the
// process tells of the view unasked comes as changes, numbered, to everyone who waits.
TRIAL("Browsing: a view taken around, clicked, scrolled, typed into and zoomed; its changes told to all who wait") {
    Bench bench;
    const auto browser = bench.browser();
    const auto view = tal::wait(bench.loop, browser->opened("file:///a.html", Browsing{}, Size{40, 30}));
    tal::docs::Change seen = tal::wait(bench.loop, view->changed(0));

    SAME(seen.address, std::string{"file:///a.html"});
    OUGHT(seen.back && !seen.loading && !seen.lost && seen.refusals.empty());

    const auto heard = [&](auto work) {
        tal::wait(bench.loop, work());
        seen = tal::wait(bench.loop, view->changed(seen.sequence));

        return seen.title;
    };

    SAME(heard([&] { return view->went("file:///b.html"); }), std::string{"go 0 file:///b.html"});
    SAME(seen.address, std::string{"file:///b.html"});
    SAME(heard([&] { return view->stepped(tal::docs::Step::back); }), std::string{"go 1"});
    SAME(heard([&] { return view->stepped(tal::docs::Step::stop); }), std::string{"go 4"});
    SAME(heard([&] { return view->clicked(1.5, 29.5); }), std::string{"click 1.500000 29.500000"});
    SAME(heard([&] { return view->wheeled(0, -48); }), std::string{"wheel 0.000000 -48.000000"});
    SAME(heard([&] { return view->keyed(tal::Key{U'\u017C'}); }), std::string{"type \xC5\xBC"});
    SAME(heard([&] { return view->keyed(tal::Key{tal::Code::enter, tal::Mods{.shift = true}}); }), std::string{"key 1 0 1"});
    SAME(heard([&] { return view->keyed(tal::Key{U'a', tal::Mods{.ctrl = true}}); }), std::string{"key 0 97 2"});
    SAME(heard([&] { return view->keyed(tal::Key{U'a', tal::Mods{.alt = true}}); }), std::string{"key 0 97 4"});
    SAME(refusal([&] { tal::wait(bench.loop, view->clicked(1, 30)); }), std::string{"other: A point outside the view."});
    SAME(heard([&] { return view->typed("za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87"); }), std::string{"type za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87"});
    SAME(heard([&] { return view->zoomed(1.5); }), std::string{"zoom 1.500000"});
    SAME(refusal([&] { tal::wait(bench.loop, view->went("https://example.org/")); }), std::string{"refused: The view may not go there."});
    SAME(refusal([&] { tal::wait(bench.loop, view->clicked(40, 1)); }), std::string{"other: A point outside the view."});
    SAME(refusal([&] { tal::wait(bench.loop, view->keyed(tal::Key{tal::Code::f5})); }), std::string{"other: A key a page does not take."});
    SAME(refusal([&] { tal::wait(bench.loop, view->typed(std::string{"\xFF"})); }), std::string{"other: Text that is not UTF-8."});
    SAME(refusal([&] { tal::wait(bench.loop, view->zoomed(6)); }), std::string{"other: A zoom out of range."});

    const auto later = [&]() -> tal::Task<void> { co_await view->typed("x"); };
    const auto [one, two, done] = tal::wait(bench.loop, tal::all(view->changed(seen.sequence), view->changed(seen.sequence), [&]() -> tal::Task<int> {
                                                co_await later();
                                                co_return 0;
                                            }()));

    SAME(one.title, std::string{"type x"});
    SAME(two, one);
    (void)done;
}

// What a process tells unasked is held to the protocol too: twenty refusals keep the last sixteen
// and count the rest; a change of a view the process never had, or a title over 64 KiB, kills the
// process, and whoever waits for a change gets broken; a view lost says so; a view closed ends
// the waiting for its changes.
TRIAL("Browsing: refusals kept and counted, events of strangers and of excess kill the process, a view lost or closed") {
    Bench bench;
    const auto flooded = tal::wait(bench.loop, bench.browser({"flood"})->opened("file:///a", Browsing{}, Size{4, 4}));
    tal::docs::Change seen = tal::wait(bench.loop, flooded->changed(0));

    while (seen.sequence < 21) seen = tal::wait(bench.loop, flooded->changed(seen.sequence));

    seen = tal::wait(bench.loop, flooded->changed(0));

    SAME(seen.sequence, std::uint64_t{21});
    SAME(seen.refusals.size(), std::size_t{16});
    SAME(seen.skipped, std::uint64_t{4});
    OUGHT(seen.refusals.front() == (tal::docs::Refusal{tal::docs::Refusal::Reason::outside, "https://example.test/4"}));
    OUGHT(seen.refusals.back() == (tal::docs::Refusal{tal::docs::Refusal::Reason::outside, "https://example.test/19"}));
    SAME(tal::wait(bench.loop, flooded->changed(seen.sequence - 1)).refusals.size(), std::size_t{1});

    for (const std::string word : {"stranger", "long", "flag", "reason", "numbered"}) {
        const auto view = tal::wait(bench.loop, bench.browser({word})->opened("file:///a", Browsing{}, Size{4, 4}));

        SAME(kind_of([&] {
                 for (std::uint64_t after = 0;; after = tal::wait(bench.loop, view->changed(after)).sequence) {}
             }),
             std::optional<Kind>{Kind::broken});
    }

    const auto lost = tal::wait(bench.loop, bench.browser({"lose"})->opened("file:///a", Browsing{}, Size{4, 4}));
    const std::uint64_t first = tal::wait(bench.loop, lost->changed(0)).sequence;

    tal::wait(bench.loop, lost->clicked(1, 1));
    OUGHT(tal::wait(bench.loop, lost->changed(first)).lost);

    std::shared_ptr<tal::docs::Browsed> closing = tal::wait(bench.loop, bench.browser()->opened("file:///a", Browsing{}, Size{4, 4}));
    tal::docs::Browsed* held = closing.get();
    const std::uint64_t now = tal::wait(bench.loop, held->changed(0)).sequence;

    SAME(refusal([&] {
             const auto [change, done] = tal::wait(bench.loop, tal::all(held->changed(now), [&]() -> tal::Task<int> {
                                                        closing.reset();
                                                        co_return 0;
                                                    }()));

             (void)change;
             (void)done;
         }),
         std::string{"broken: The view was closed."});
}

// An engine that says before its hello that it is not there gives its reason, which a chain of
// browsers puts in its sentence with the reasons of the others; one that speaks version 1 is not
// there either.
TRIAL("Browsing: an engine not there says why, and a chain says the reasons of all") {
    Bench bench;

    SAME(refusal([&] { (void)tal::wait(bench.loop, bench.browser({"unavailable=WebView2 Runtime is not installed."})->opened("a", Browsing{}, Size{1, 1})); }),
         std::string{"unsupported: WebView2 Runtime is not installed."});
    SAME(refusal([&] { (void)tal::wait(bench.loop, bench.browser({"old"})->opened("a", Browsing{}, Size{1, 1})); }),
         std::string{"unsupported: The renderer toy did not greet: it speaks version 1."});

    const auto chain = std::make_shared<tal::docs::Browsers>(
        std::vector<std::shared_ptr<tal::docs::Browser>>{bench.browser({"unavailable=WebKitGTK 6 is not installed."}), bench.browser()});

    SAME(refusal([&] { (void)tal::wait(bench.loop, chain->opened("a", Browsing{true, false}, Size{1, 1})); }),
         std::string{"unsupported: WebKitGTK 6 is not installed. The renderer toy runs no scripts."});
#if !defined(TAL_WEBKITGTK) && !defined(TAL_WEBVIEW2)
    SAME(refusal([&] { (void)tal::wait(bench.loop, tal::docs::Browser::builtin()->opened("a", Browsing{}, Size{1, 1})); }),
         std::string{"unsupported: This program was built without a web engine."});
#endif
}

// and opens the page in the first that can; when none can, unsupported; any other trouble of a
// link ends the search with it. A chain of renderers opens a document by the first link that
// takes its head.
TRIAL("Chains: browsers past those that cannot, renderers by the head") {
    Bench bench;

    bench.limits.greeting = 1000ms;

    const auto chain = std::make_shared<tal::docs::Browsers>(
        std::vector<std::shared_ptr<tal::docs::Browser>>{bench.browser({"die-first"}), bench.browser(), bench.browser({"scripts"})});

    SAME(chain->name(), std::string_view{"browsers"});

    const auto page = tal::wait(bench.loop, chain->opened("a", Browsing{true, false}, Size{1, 1}));

    SAME(tal::wait(bench.loop, page->shot()).title, std::string{"scripts=1 network=0"});
    SAME(refusal([&] { (void)tal::wait(bench.loop, chain->opened("a", Browsing{true, true}, Size{1, 1})); }),
         std::string{"unsupported: The renderer toy is not there. The renderer toy runs no scripts. The renderer toy has no network."});
    SAME(refusal([&] { (void)tal::wait(bench.loop, tal::docs::Browsers{{}}.opened("a", Browsing{}, Size{1, 1})); }),
         std::string{"unsupported: No browser can open the page."});
    SAME(refusal([] { (void)tal::docs::Browsers{std::vector<std::shared_ptr<tal::docs::Browser>>{nullptr}}; }), std::string{"other: A link of no browser."});

    const auto broken = std::make_shared<tal::docs::Browsers>(
        std::vector<std::shared_ptr<tal::docs::Browser>>{std::make_shared<Failing>(Kind::unsupported), std::make_shared<Failing>(Kind::broken), bench.browser()});

    SAME(refusal([&] { (void)tal::wait(bench.loop, broken->opened("a", Browsing{}, Size{1, 1})); }), std::string{"broken: It fails."});

    const auto scripted = std::make_shared<tal::docs::ScriptedRenderer>(std::vector<Page>{{10, 20}});
    const auto pdf = bench.renderer();
    Command program;

    program.program = tal::Path{"toy"};

    const auto picky = tal::docs::renderer(
        program, [](std::span<const std::byte> head) { return head.size() >= 5 && std::to_integer<char>(head[0]) == '%'; }, {},
        tal::Port<tal::Processes>{bench.processes});
    const auto renderers = std::make_shared<tal::docs::Renderers>(std::vector<std::shared_ptr<tal::docs::Renderer>>{picky, scripted}, "mine");

    SAME(renderers->name(), std::string_view{"mine"});
    OUGHT(renderers->takes(bytes_of("%PDF-")));
    SAME(bench.opened(renderers, "%PDF-")->pages().size(), std::size_t{1});
    SAME(scripted->opening, std::size_t{0});
    OUGHT(bench.opened(renderers, "plain")->pages() == (std::vector<Page>{{10, 20}}));
    SAME(scripted->opening, std::size_t{1});

    const auto none = std::make_shared<tal::docs::Renderers>(std::vector<std::shared_ptr<tal::docs::Renderer>>{picky});

    OUGHT(!none->takes(bytes_of("plain")));
    SAME(refusal([&] { (void)bench.opened(none, "plain"); }), std::string{"unsupported: No renderer takes the document."});
    SAME(refusal([] { (void)tal::docs::Renderers{std::vector<std::shared_ptr<tal::docs::Renderer>>{nullptr}}; }), std::string{"other: A link of no renderer."});
}

// A password goes to the process with the opening: none, or another one than the document's, is
// denied in the process's words, and the process lives on to open the document with the right
// one. A chain hands the password to the link that takes the document.
TRIAL("Rendering: a password opens a document, none or another is denied") {
    Bench bench;

    const auto locked = bench.renderer({"password=secret"});

    SAME(refusal([&] { (void)bench.opened(locked, "pages:2"); }), std::string{"denied: The document needs a password."});
    SAME(refusal([&] { (void)bench.opened(locked, "pages:2", "guess"); }), std::string{"denied: The password does not open the document."});
    SAME(bench.opened(locked, "pages:2", "secret")->pages().size(), std::size_t{2});

    const auto scripted = std::make_shared<tal::docs::ScriptedRenderer>();

    scripted->password = "other";

    const auto chain = std::make_shared<tal::docs::Renderers>(std::vector<std::shared_ptr<tal::docs::Renderer>>{scripted});

    SAME(refusal([&] { (void)bench.opened(chain, "plain", "secret"); }), std::string{"denied: The password does not open the document."});
    SAME(bench.opened(chain, "plain", "other")->pages().size(), std::size_t{1});
}

// An engine let go kills its process at once: the toy behind it is gone, whatever it was doing.
TRIAL("Rendering: an engine let go kills its process") {
    Bench bench;

    UNTIL(bench.loop, toys.load() == 0);

    auto renderer = bench.renderer({"render=hang"});
    auto document = bench.opened(renderer);

    UNTIL(bench.loop, toys.load() == 1);
    document.reset();
    renderer.reset();
    UNTIL(bench.loop, toys.load() == 0);

    auto browser = bench.browser();
    auto page = tal::wait(bench.loop, browser->opened("a", Browsing{}, Size{1, 1}));

    UNTIL(bench.loop, toys.load() == 1);
    browser.reset();
    OUGHT(toys.load() == 1);
    page.reset();
    UNTIL(bench.loop, toys.load() == 0);
}

// The scripted adapters of the trials: pages in the tone of their number, a page at the size
// asked, documents locked by a password, the addresses and ways a browser was asked for.
TRIAL("Scripted renderer and browser") {
    tal::Loop loop;
    tal::docs::ScriptedRenderer renderer;
    tal::docs::ScriptedBrowser browser;
    const auto document = tal::wait(loop, renderer.opened(std::make_shared<tal::meta::Held>(Bytes{}), {}));
    const Pixels page = tal::wait(loop, document->rendered(0, Size{2, 2}, Region{}));

    OUGHT(document->pages() == (std::vector<Page>{{612, 792}}));
    OUGHT(first_pixel(page) == (std::vector<int>{0, 0, 0, 255}));
    SAME(renderer.opening, std::size_t{1});

    renderer.password = "secret";
    SAME(refusal([&] { (void)tal::wait(loop, renderer.opened(std::make_shared<tal::meta::Held>(Bytes{}), {})); }),
         std::string{"denied: The document needs a password."});
    SAME(refusal([&] { (void)tal::wait(loop, renderer.opened(std::make_shared<tal::meta::Held>(Bytes{}), "guess")); }),
         std::string{"denied: The password does not open the document."});
    SAME(tal::wait(loop, renderer.opened(std::make_shared<tal::meta::Held>(Bytes{}), "secret"))->pages().size(), std::size_t{1});
    SAME(renderer.opening, std::size_t{4});

    const auto shown = tal::wait(loop, browser.opened("file:///b", Browsing{true, false}, Size{2, 1}));

    SAME(tal::wait(loop, shown->shot()).title, std::string{"file:///b"});
    OUGHT(browser.addresses == (std::vector<std::string>{"file:///b"}));
    OUGHT(browser.ways.size() == 1 && browser.ways[0].scripts && !browser.ways[0].network);
}

// The toy as a real process (this binary pretending): it renders, a hang is killed after the
// limit, a death breaks what waits and the next opening starts it anew, and one that listens no
// more is killed when its engine is let go; held to a limit of memory, the process cannot take
// 512 MiB and ends (not under the sanitizers, whose own reservations the limit would count).
TRIAL("Rendering: a real process rendered, killed, started anew, held to its memory") {
    tal::Loop loop;
    tal::docs::Limits limits;

    limits.answer = 1500ms;

    const auto real = [&limits](std::vector<std::string> words, std::uint64_t memory = 0) {
        Command program;

        program.program = tal::testing::mine();
        program.arguments = {"--pretending", "toy-renderer"};
        program.arguments.insert(program.arguments.end(), words.begin(), words.end());
        program.memory = memory;

        return tal::docs::renderer(program, [](std::span<const std::byte>) { return true; }, limits);
    };
    const auto opened = [&loop](const std::shared_ptr<tal::docs::Renderer>& renderer) {
        return tal::wait(loop, renderer->opened(std::make_shared<tal::meta::Held>(bytes_of("pages:2")), {}));
    };

    const auto fine = opened(real({}));

    OUGHT(first_pixel(tal::wait(loop, fine->rendered(1, Size{2, 2}, Region{}))) == (std::vector<int>{1, 0, 255, 255}));

    const auto hanging = real({"render=hang"});

    SAME(kind_of([&] { (void)tal::wait(loop, opened(hanging)->rendered(0, Size{1, 1}, Region{})); }), std::optional<Kind>{Kind::overdue});
    SAME(opened(hanging)->pages().size(), std::size_t{2});

    const auto dying = real({"render=die"});

    SAME(kind_of([&] { (void)tal::wait(loop, opened(dying)->rendered(0, Size{1, 1}, Region{})); }), std::optional<Kind>{Kind::broken});
    SAME(opened(dying)->pages().size(), std::size_t{2});

    const std::filesystem::path pidfile = std::filesystem::temp_directory_path() / ("tal-toy-" + std::to_string(own_number()) + ".pid");
    auto deaf = real({"deaf", "pidfile=" + pidfile.string()});
    auto document = opened(deaf);
    Stray stray;

    std::ifstream{pidfile} >> stray.number;
    std::filesystem::remove(pidfile);

    const long long number = stray.number;

    OUGHT(number > 0 && running(number));
    document.reset();
    deaf.reset();
    SOON(!running(number));

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    const auto free = real({"render=hog"});

    OUGHT(first_pixel(tal::wait(loop, opened(free)->rendered(0, Size{1, 1}, Region{})))[3] == 255);

    const auto held = real({"render=hog"}, std::uint64_t{256} << 20);

    SAME(kind_of([&] { (void)tal::wait(loop, opened(held)->rendered(0, Size{1, 1}, Region{})); }), std::optional<Kind>{Kind::broken});
#endif
}
