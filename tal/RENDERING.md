# The rendering protocol

tal renders documents (PDF and the like) and web pages in a separate process, so that a hostile file
or page can at worst bring down that process, never the program. `tal::docs::renderer` and
`tal::docs::browser` turn any program that speaks this protocol into an adapter of the ports
`Renderer` and `Browser`; this file is the contract such a program keeps. It describes version 2; the program speaks no other.

## Frames

The program and the process talk over the process's standard input (program to process) and
standard output (process to program). Its standard error is read and thrown away, so the process may
write there freely; it must write nothing but frames to its standard output.

Every message is a frame:

| Bytes | Field |
|---|---|
| 4 | length of the rest of the frame (at least 5) |
| 1 | sort |
| 4 | id: the number of the question the frame asks or answers |
| length − 5 | payload |

Numbers are unsigned and little-endian. In payloads, `u8`, `u32` and `u64` are such numbers of 1, 4
and 8 bytes; `f64` is an IEEE 754 double in 8 bytes, little-endian, and always finite; `text` is a
`u32` length followed by that many bytes of UTF-8 (broken UTF-8 is repaired to U+FFFD on reading).

## Sorts

| Sort | Value | Sent by | Meaning |
|---|---|---|---|
| hello | 1 | process | the first frame, once |
| done | 2 | process | a question answered |
| failed | 3 | process | a question that could not be answered |
| read | 4 | process | a question for bytes of a document |
| changed | 5 | process | a view changed (unasked, id 0) |
| refused | 6 | process | a view refused something (unasked, id 0) |
| lost | 7 | process | a view lost its page (unasked, id 0) |
| open | 10 | program | open a document |
| render | 11 | program | render a page of an open document |
| close | 12 | program | close a document |
| navigate | 13 | program | open a view of an address |
| shot | 14 | program | a picture of a view |
| resize | 15 | program | change the size of a view |
| dismiss | 16 | program | close a view |
| letters | 17 | program | the letters of a page of an open document |
| block | 20 | program | bytes of a document, answering a read |
| unread | 21 | program | a read that could not be answered |
| go | 30 | program | take a view to an address, back, forward, again, or stop it |
| click | 31 | program | a click in a view |
| wheel | 32 | program | scroll a view |
| key | 33 | program | a key pressed in a view |
| type | 34 | program | text typed into a view |
| zoom | 35 | program | zoom a view |

The two sides number their own questions independently: the program numbers open, render, letters,
close, navigate, shot, resize, dismiss, go, click, wheel, key, type and zoom; the process numbers
its reads. What the process tells of its views unasked (changed, refused, lost) carries id 0 and is
answered by nothing. An answer carries the id of the
question it answers. The program may ask several questions before the first is answered, and the
process may answer them in any order.

## Hello

The process speaks first, before it reads anything:

    hello (id 0): u32 version (2), text engine, u8 abilities

`engine` names the engine and its version, for people. `abilities` is the sum of:

| Ability | Value |
|---|---|
| documents (open, render, close) | 1 |
| pages (navigate, shot, resize, dismiss) | 2 |
| scripts in pages | 4 |
| network in pages | 8 |
| letters of the pages of documents | 16 |

A process whose engine cannot work here (a library or a runtime missing) says so before its hello,
and ends:

    failed (id 0): u8 kind (unsupported, 8), text sentence (for people, as "WebView2 Runtime is not
                   installed.")

A process that ends or keeps silent before its hello, or greets with another version, is taken as an
engine that is not there: the program does not start it again while it lives, and a chain of
adapters goes on to the next one.

## Documents

    open:   u32 document, u64 size, text password (empty for none)
    done:   u32 count, then for each page f64 width, f64 height (in points, 1/72 inch, both > 0,
            as the page is shown, turned by its rotation)

The program chooses the number of the document; it stays the program's name for it in every later
frame. A document that needs a password fails as denied, with "The document needs a password." when
none was given and "The password does not open the document." for another one. The process should
keep the password no longer than it needs it.

While opening (and later, while rendering), the process reads the document only by asking:

    read:   u32 document, u64 at, u32 much (at most 1 MiB)
    block:  the bytes (fewer than asked at the end of the document)
    unread: u8 kind, text sentence (as in failed)

The process needs no access to any file, and should have none.

    render: u32 document, u32 page (from 0), u32 width, u32 height,
            f64 left, f64 top, f64 region width, f64 region height
    done:   u32 width, u32 height, then width × height × 4 bytes of RGBA, straight alpha, rows top
            to bottom

The region is the part of the page to render, in fractions of the page's size: within [0, 1], with an
area. The picture must have exactly the width and height asked.

    letters: u32 document, u32 page (from 0)
    done:    u32 count, then for each letter u32 code (a Unicode character), f64 left, f64 top,
             f64 width, f64 height

Only a process that claims letters is asked for them. The letters of a page come in the order of its
text, spaces and line ends among them as the engine knows them; each box is where the letter stands,
in fractions of the page as it is shown (turned by its rotation): within [0, 1], of a size of zero or
more (a letter the engine adds, as a line end, may have none).

    close:  u32 document
    done:   empty

## Pages

    navigate: u32 view, text address, u8 scripts (0 or 1), u8 network (0 or 1), u32 width, u32 height
    done:     empty

A process must keep to scripts and network as asked; one that cannot switch them off must not claim
the ability, and the program then refuses to open such a view.

    shot:     u32 view
    done:     text title, text address, then the picture as in render, of the view's size
    resize:   u32 view, u32 width, u32 height
    done:     empty
    dismiss:  u32 view
    done:     empty

The views are numbered by the program apart from the documents. A view goes on by itself only where
its Browsing allows (with the network off, nowhere but `file:` addresses of no host or of localhost;
`javascript:` never; `data:` and `blob:` never as the page itself); anything else is refused and told.
New windows, downloads, dialogs of scripts, choosing files and asking for permissions are always
refused. Pages from `file:` reach no other files from scripts.

    go:       u32 view, u8 how (0 address, 1 back, 2 forward, 3 again, 4 stop), text address (for 0
              only)
    done:     empty, once the process has taken the way (a way within the same document too); the
              page loads after, and says so as changes. A go not yet answered when the next one
              comes fails at once. An address the view may not reach fails as refused.
    click:    u32 view, f64 x, f64 y (pixels of the view)
    wheel:    u32 view, f64 dx, f64 dy (pixels)
    key:      u32 view, u8 key, u32 letter (0 for a named key), u8 modifiers (Shift 1, Ctrl 2, Alt 4)
    type:     u32 view, text (UTF-8)
    zoom:     u32 view, f64 factor (0.25 to 5)
    done:     empty, for each of them

The named keys are: Enter 1, Tab 2, Backspace 3, Delete 4, Escape 5, Up 6, Down 7, Left 8, Right 9,
Home 10, End 11, Page Up 12, Page Down 13. Letters without Ctrl and Alt come as type, not as key.

What the process tells of a view unasked, with id 0:

    changed:  u32 view, text title, text address, u8 loading, u8 can go back, u8 can go forward
              (each 0 or 1; the title and the address at most 64 KiB)
    refused:  u32 view, u8 reason (a new window 0, an address outside what the view may reach 1, a
              download 2, a dialog 3, a permission 4, a file to send 5), text address (at most 64 KiB)
    lost:     u32 view: the process of the page ended (its memory, a crash); the view shows nothing
              and must be opened again

The program keeps the last change of each view, at most 16 refusals (the older counted) and whether
it is lost, so a process cannot flood its memory.

## Troubles

    failed: u8 kind, text sentence

`kind` is the number of a `tal::Kind`: missing 0, denied 1, exists 2, busy 3, full 4, refused 5,
broken 6, malformed 7, unsupported 8, overdue 9, other 10. The sentence is shown to people.

## What the program holds the process to

Everything the process says is checked as the words of a stranger. The process is killed, and every
question waiting on it fails as malformed, for: a frame longer than `ceiling` × 4 + 4096 bytes
(`media::ceiling` pixels) or shorter than 5; a frame of an unknown sort, or anything before the hello;
an answer to no question; a payload shorter or longer than its sort says; something told of a view
the process was never asked to open in this run, or told with an id; a title or an address over 64
KiB, a flag other than 0 and 1, a refusal of no reason; a number that is not
finite; a picture of another size than asked; more pages than the frame holds or a page of no area; a
read of more than 1 MiB; a trouble of an unknown kind.

A question unanswered within its limit (by default 10 s, navigation 30 s) kills the process and fails
as overdue. A process that ends fails every waiting question as broken; the next question starts it
anew, and the documents and views of the process that ended are lost with it. When the program lets
go of the adapter and of everything opened through it, the process is killed; it gets no farewell.

The adapter may also hold the process to a limit of memory (`Command::memory`): a job object on
Windows, `RLIMIT_DATA` on Linux and macOS.
