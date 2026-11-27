# talcite

talcite is a C++20 library for programs in the terminal and beside it: tasks and executors, files,
processes, networking, images and documents, drawing and plots, and a tree of nodes shown in the
terminal with its widgets. It is the foundation of twa, a terminal file manager for film and music
libraries. Its library is `tal`, under `tal/`, in the namespace `tal::`.

tal holds no third-party code and needs nothing beyond the C++ standard library, threads and the
system's own libraries. Its character tables (widths, grapheme clusters) are generated from the
Unicode Character Database; see `LICENSE-UNICODE`. The names of glyphs that the text version of PDF
reads come from the Adobe Glyph List; see `LICENSE-AGL`. The named character references of the
text version of HTML are those of the WHATWG HTML Living Standard (© WHATWG, CC BY 4.0), taken from
Python's `html.entities`. What the system gives (TLS, ciphers, digests, the clipboard, notices) is
taken from the system; other implementations plug into ports, and the bridges to other people's
code (OpenSSL, pugixml, nlohmann/json, stb, miniz) live in the sibling project talcite-adapters.

## What is inside

Each part is a CMake target of its own; a program links the ones it uses.

`tal-core`
- `Task<T>` and coroutines (`co_await`, `co_return`) owned by a `Scope`, which cancels what it
  started when it goes; `first`, `all`, timeouts. Executors: `Loop`, one event loop per thread,
  and a pool; a clock, `ManualClock` for exact trials.
- Files through the port `Files`: reading, writing, listing, walking, copying, moving, the trash,
  watching folders, known folders, network shares; `fs::Memory`, files in memory for trials.
- Processes through the port `Processes`, with a scripted adapter for trials; paths, streams,
  JSON, XML, zip and tar archives (plain or through gzip, with our own inflate).
- Text: UTF-8, terminal cells, letter case, collation and the rules of languages; the texts of a
  program in its languages and its settings, both generated from JSON while it is built
  (`tal_texts`, `tal_settings`), and the rules a translation must keep (`tal::wording`); the log,
  troubles (`tal::Trouble` and its kinds).
- Digests and ciphers from the system: CNG on Windows, the kernel's AF_ALG on Linux.

`tal-net`
- Addresses and names, TCP as a stream, listeners and UDP, waited for by one reactor thread: epoll
  on Linux, kqueue on macOS and the BSDs, IOCP on Windows, poll elsewhere.
- TLS over any stream (Schannel on Windows; elsewhere an adapter), an HTTP client with the proxy of
  the environment or of WinHTTP's setting.
- What connects users: STUN, a beacon in the local network, ports opened on the router, key
  agreements (X25519, CPace for pairing by a password) and the channel of sealed messages they open.

`tal-media`
- Images as RGBA frames, scaled in linear light and decoded through the port `Decoding` (our own
  PNG, JPEG and GIF); the tags and properties of sound and video files; the structure of PDF.
- Documents and web pages rendered by a process of their own through the ports `Renderer` and
  `Browser` (see `tal/RENDERING.md`); PDFium, WebKitGTK and WebView2 only on the programmer's word.
- Drawing as on a canvas (`tal::drawing`): paths, strokes, gradients, patterns, clips and images on
  a raster through a `Pen`, rasterized by our own code, and text from TrueType fonts, DejaVu Sans
  built in. The same pen draws into a `Recording`, which plays again on any pen at any scale and
  writes itself as SVG (`svg`), its curves and text kept as curves; images, rasters among them,
  encode as PNG (`media::encoded`).

`tal-plotting`
- Plots drawn with a pen: scales straight and logarithmic with their ticks and labels, series as
  lines, marks, bars and steps, histograms, heat maps with their bar of colors, contours,
  palettes mixed in Oklab, the readout under the cursor, and traces of samples in time.

`tal-ui`
- `App` shows a `Document` in the terminal: it probes what the terminal can do (colors, the mouse,
  the alternate screen, sixel and kitty graphics) and reads input in every form terminals send it.
- The document is a tree of nodes (`Node`), laid out by `Row`, `Column`, `Grid`, `Align` and
  `Split` (its room shared by parts, the borders dragged with the mouse), styled by classes with
  parameters (no cascade), told of events through `on<E>` and `Hook`; loads carried with the mouse
  from node to node (`dragged`, `DragOver`, `Dropped`).
- Widgets: labels, buttons, ticks, choices, selects, gauges, fields with history, lists, tables,
  trees, scrolling, windows with their layers, messages, forms and a palette of actions found by
  their letters, menus with a menu bar, a key bar and tabs (also in the edge of a frame), a text editor, a diff, images, the pages of documents (`Leaves`), web pages
  (`Webview`), a terminal emulator, and `Drawing` with `Simulation` for pictures that move; a
  drawing records a frame of what it shows (`Drawing::recorded`), for SVG or a larger PNG.

`tal-testing`, `tal-scene`
- The runner of the trials, with a watch that writes the stacks of a hung run, and a document
  driven by hand on a manual clock for the trials of programs.

## Platforms

tal is written against C++20 and builds with GCC, Clang and MSVC. Where a platform or an older
standard library lacks something, the code takes another way at compile time or at run time, so
the interface stays the same everywhere.

- Windows 10 and 11, and also Windows 7 and 8.1: the console without VT sequences is translated,
  and what the system lacks is detected while the program runs.
- Linux (epoll, inotify, AF_ALG digests).
- FreeBSD and other BSDs, and macOS. The macOS paths have not yet been tested on a real Mac.

## Building

Requirements: CMake 3.21 or newer and a C++20 compiler.

```sh
cmake -S . -B build -DTALCITE_TESTS=ON
cmake --build build
./build/bin/tal-test
```

Options:

| Option | Default | Meaning |
|---|---|---|
| `TALCITE_TESTS` | `OFF` | Build the trials, `tal-test`. |
| `TALCITE_DEMOS` | `ON` | Build the demo programs. |
| `TALCITE_LEGACY_CONSOLE` | `OFF` | Translate the Windows console even when it accepts VT, to try the Windows 7 path on newer systems. |
| `TAL_OWN_SUMS` | `OFF` | Our own digests where the system gives none. |
| `TAL_WEAK_SUMS` | `OFF` | The digests MD5 and SHA-1. |
| `TAL_OWN_CURVE` | `OFF` | Our own arithmetic of curve 25519, for CPace. |
| `TAL_REMEMBER_LOGINS` | `OFF` | Logins of shares remembered by the system (Windows). |
| `TAL_NET_KQUEUE` | `OFF` | Wait for sockets through kqueue where the system has another way (libkqueue on Linux). |
| `TAL_PDFIUM` | `OFF` | Render PDF through PDFium in a process of its own (fetches PDFium, checked by its SHA-256; on Windows the program carries the library and unpacks it, checked, into the user's local data). |
| `TAL_WEBKITGTK` | `OFF` | Show web pages through WebKitGTK 6 in a process of its own (Linux). |
| `TAL_WEBVIEW2` | `OFF` | Show web pages through WebView2 in a process of its own (Windows). |
| `TAL_FUZZ` | `OFF` | Build the fuzz targets (Clang only; instruments the whole build). |

Whatever weakens security is built in only on the programmer's word: without its option that code
is not in the program.

The library has no install rules yet; a program uses it as a subdirectory:

```cmake
add_subdirectory(path/to/talcite talcite)
target_link_libraries(my-program PRIVATE tal-ui)
```

## Trials

`tal-test` runs every trial and ends with `Passed N of M.`; its exit code is 0 only when all of
them pass.

```sh
tal-test --list                # list the trials with their files
tal-test --file plotting.cpp   # run the trials of one file
tal-test --only "<name>"       # run one trial by its exact name
```

With `TAL_FUZZ`, libFuzzer drives the readers of hostile input (images, fonts, the text of PDF and
HTML, archives, JSON, XML, HTTP, the terminal emulator and others) and the drawing and the plots,
under AddressSanitizer and UBSan.

## Demos

`tal-pendulum` is a double pendulum drawn in the terminal beside plots of its energy, its angles
and its phase. A body can be dragged with the mouse; space pauses, `s` steps, `+` and `-` change
the pace, the mouse reads the plots and a click holds them (`f` chooses what it holds), `h` shows
the histogram of the second angle, `m` the map of the potential energy, `p` keeps the drawing as
SVG and as PNG twice as large in the folder of pictures, `q` quits.

`tal-hello` is the smallest program:

<!-- tal/demo/hello.cpp -->
```cpp
#include "tal/app.hpp"
#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/layout.hpp"

// tal-hello, the smallest program on tal and the example of the README: a document holding a
// column of two labels, shown in the terminal until q or F10. The application is made first and
// the document after it, so the document goes first at the end, which tal allows.
int main() {
    tal::App app;
    tal::Document document;
    tal::Column& column = document.children().add<tal::Column>(1);

    column.children().add<tal::Label>("Hello from tal.");
    column.children().add<tal::Label>("q or F10 quits.");

    const tal::Hook keys = document.on<tal::KeyEvent>([&app](tal::KeyEvent& event) {
        if (event.key == tal::Key{U'q'} || event.key == tal::Key{tal::Code::f10}) app.stop();
    });

    app.show(document);

    return app.run();
}
```

`python3 scripts/readme.py` checks that this example is the program as it is.

## Ports and adapters

What a program may want to replace, in trials or for another implementation, it reaches through a
port, `Port<T>`: given one, or else the default, which is the system's (`T::builtin()`).

| Port | What it gives | Default | For trials |
|---|---|---|---|
| `Clock` | the time and the timers | the system's | `ManualClock` |
| `Console` | the terminal | the system's | `ScriptedConsole` |
| `Clipboard` | the clipboard | the system's, or the terminal's (OSC 52) | `ScriptedClipboard` |
| `Files` | the filesystem | the system's | `fs::Memory` |
| `Processes` | starting programs | the system's | `Scripted` |
| `Notifier` | notices to the user | the system's, or the terminal's | `ScriptedNotifier` |
| `net::Tls` | TLS | Schannel on Windows; none elsewhere | |
| `Cipher` | key agreement and sealing | CNG on Windows; none elsewhere | |
| `media::Decoding` | decoding images | our PNG, JPEG and GIF | |
| `docs::Renderer` | rendering documents | PDFium, when built | `ScriptedRenderer` |
| `docs::Browser` | showing web pages | WebKitGTK or WebView2, when built | `ScriptedBrowser` |

Where the default is none, the program gives an adapter, as talcite-adapters will for OpenSSL; any
program that speaks the protocol of `tal/RENDERING.md` can render documents or show web pages.

## The old library

`talcite/`, the library under the namespace `talcite::` that twa is built on, stays beside tal,
with its own trials (`talcite-test`) and demos, until twa moves over to tal; then it goes.

## License

MIT. Copyright (c) 2026 hv6erT. See [LICENSE](LICENSE). Co-written by Claude AI (made by Anthropic).
