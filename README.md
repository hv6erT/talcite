# talcite

talcite is a C++20 library for building terminal programs: an asynchronous core, a filesystem
layer, networking, process control and a set of terminal UI widgets in the style of Midnight
Commander. It is the foundation of twa, a terminal file manager for film and music libraries.

The library has no third-party code and no required dependencies beyond the C++ standard library
and threads. Everything that needs an outside implementation (TLS, ciphers, image decoders, parsers,
embedded tags) is plugged in through small interfaces; ready implementations live in the
sibling project talcite-adapters.

## What is inside

Asynchronous core
- `Promise<T>` and `SharedPromise<T>`: work starts at once and lasts while someone wants its result;
  the last owner to let go cancels it. Chains with `then`, `recover`, `finally`, `on`, timeouts,
  outcomes and progress; joins `all`, `settled`, `race`, `any`; `Settle` for event-driven
  producers; coroutines (`co_await`, `co_return`).
- Places where code runs: `Loop` (one event loop per thread) and `Pool` (a thread pool with separate
  lanes for I/O and CPU work), plus `ManualPlace` and `ManualClock` for deterministic tests; `Bell`,
  `Time` and `Stopwatch`.

Filesystem (`talcite::fs`)
- Asynchronous queries and changes: listing, walking, reading, writing, copying, moving, erasing,
  links, permissions, attributes, free space, volumes and mount points.
- Directory watching, known folders, network shares (SMB), a single-instance lock (`Claim`) and an
  antivirus check of received files through the system's scanner.

Networking (`talcite::net`)
- TCP and local streams, listeners, UDP, an HTTP/1.1 client with redirects, keep-alive and proxies.
- NAT traversal: STUN, TURN relays, hole punching, router port mapping (PCP, NAT-PMP, UPnP IGD)
  and LAN discovery by broadcast.
- Sealed channels: key agreement (X25519, CPace for password-based pairing) and AES-256-GCM
  messages over any stream.

Processes and system
- `Child`: starting programs with arguments, environment and working directory, collecting their
  output or handing them the terminal.
- Autostart at login, the clipboard, the console, UTF-8 command-line arguments, the system
  language, random bytes from the operating system.

Terminal and widgets (`talcite::ui`)
- `Term` and `Capabilities`: drawing on the terminal and probing what it supports (colors, mouse,
  alternate screen, sixel and kitty graphics).
- A tree of nodes with events (keys, mouse, paste, resize) and layout containers (`Column`, `Row`,
  `Stack`, `Box`, `Scroll`, `Frame`).
- Widgets: spans, buttons, fields with history, check boxes, choices, drop-down lists, lists,
  tables, gauges, menus with a menu bar, a key legend, windows, dialogs and message boxes, a
  streaming viewer, a multi-window editor, a diff viewer and an image widget (sixel or block
  characters).
- Skins: colors and frame characters for every role, switchable at run time.

Utilities
- UTF-8 decoding and display width, Base64, JSON and XML trees, text search with regular
  expressions, digests (MD5, SHA-1, SHA-256, SHA-512), HMAC and HKDF.
- `Collation`: alphabetical order as a language expects it, for Latin, Greek and Cyrillic, with
  the letters of Polish, Czech, Slovak and Spanish in their own places, numbers read as numbers
  ("Part 2" before "Part 10"), and sort keys that compare as plain bytes.

## Platforms

talcite is written against C++20 and builds with GCC, Clang and MSVC. Where a platform or an older
standard library lacks something, the code selects a fallback at compile time or at run time, so
the interface stays the same everywhere.

- Windows 10 and 11, and also Windows 7 and 8.1: the console without VT sequences, named pipes in
  place of AF_UNIX and a portable X25519 are used where the system lacks them.
- Linux (epoll, inotify, AF_ALG digests).
- FreeBSD and other BSDs, and macOS. The macOS paths have not yet been tested on a real Mac.

Digests come from the system: CNG on Windows and the kernel's AF_ALG socket on Linux. Where the
system provides none (macOS, BSD, a container that blocks AF_ALG), a spare implementation can be
given with `talcite::spared(...)`, for example the OpenSSL one from talcite-adapters.

## Building

Requirements: CMake 3.21 or newer and a C++20 compiler.

```sh
cmake -S . -B build -DTALCITE_TESTS=ON
cmake --build build
./build/bin/talcite-test
```

Options:

| Option | Default | Meaning |
|---|---|---|
| `TALCITE_TESTS` | `OFF` | Build the test program `talcite-test`. |
| `TALCITE_DEMOS` | `ON` | Build the demo programs `talcite-<name>`. |
| `TALCITE_LEGACY_CONSOLE` | `OFF` | Translate the Windows console even when it accepts VT, to test the Windows 7 path on newer systems. |

The library has no install rules yet; a program uses it as a subdirectory:

```cmake
add_subdirectory(path/to/talcite talcite)
target_link_libraries(my-program PRIVATE talcite)
```

## Tests

`talcite-test` runs every trial and ends with `Passed N of M.`; its exit code is 0 only when all
of them pass. `ctest` runs it too.

```sh
talcite-test --list                # list the trials with their files
talcite-test --file promise.cpp    # run the trials of one file
talcite-test --only "<name>"       # run one trial by its exact name
```

## Demos

Each file in `demo/` becomes a program `talcite-<name>`: `text`, `scroll`, `list`, `menu`,
`legend`, `dialog`, `message`, `select`, `gauge`, `history`, `skin`, `keys`, `editor`, `viewer`,
`image`, `child`, `terminal`, `net`, `many`, `stunning` and `punching`. For example,
`talcite-keys` prints every key, mouse, paste and resize event the terminal sends, and
`talcite-terminal` shows what the capability probe found.

A minimal program:

```cpp
#include <memory>

#include "talcite/frame.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"

class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();
        return true;
    }

private:
    talcite::Term& term_;
};

int main() {
    talcite::Term term;
    Sheet sheet{term};

    const auto frame = std::make_shared<talcite::ui::Frame>("hello");
    frame->footer("F10 quits");
    frame->children().add(std::make_shared<talcite::ui::Span>("Hello from talcite."));
    sheet.children().add(frame);

    term.show(sheet);
    term.run();
}
```

## Plugging in implementations

An implementation derives from one of the provider interfaces and is passed around in a
`Seam<T>`:

| Interface | Used by | Example implementation |
|---|---|---|
| `net::TlsProvider` | `TlsStream`, `HttpClient` | OpenSSL, Schannel |
| `CipherProvider` | key agreement, `Channel` | OpenSSL, CNG |
| `ImageProvider` | `Pixels`, `ui::Image` | stb_image, the GIF decoder |
| `MetaProvider` | embedded tags of audio files | talcite tags |
| `Codec<xml::Document>`, `Codec<json::Value>` | parsing and writing | pugixml, nlohmann/json |
| `DigestProvider` | `talcite::spared(...)` | OpenSSL |

All of these are in talcite-adapters.

## License

MIT. Copyright (c) 2026 hv6erT. See [LICENSE](LICENSE). Co-written by Claude AI (made by Anthropic).
