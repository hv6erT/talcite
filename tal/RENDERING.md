# The rendering protocol

tal renders documents (PDF and the like) and web pages in a separate process, so that a hostile file
or page can at worst bring down that process, never the program. `tal::docs::renderer` and
`tal::docs::browser` turn any program that speaks this protocol into an adapter of the ports
`Renderer` and `Browser`; this file is the contract such a program keeps. It describes version 1.

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
| open | 10 | program | open a document |
| render | 11 | program | render a page of an open document |
| close | 12 | program | close a document |
| navigate | 13 | program | open a view of an address |
| shot | 14 | program | a picture of a view |
| resize | 15 | program | change the size of a view |
| dismiss | 16 | program | close a view |
| block | 20 | program | bytes of a document, answering a read |
| unread | 21 | program | a read that could not be answered |

The two sides number their own questions independently: the program numbers open, render, close,
navigate, shot, resize and dismiss; the process numbers its reads. An answer carries the id of the
question it answers. The program may ask several questions before the first is answered, and the
process may answer them in any order.

## Hello

The process speaks first, before it reads anything:

    hello (id 0): u32 version (1), text engine, u8 abilities

`engine` names the engine and its version, for people. `abilities` is the sum of:

| Ability | Value |
|---|---|
| documents (open, render, close) | 1 |
| pages (navigate, shot, resize, dismiss) | 2 |
| scripts in pages | 4 |
| network in pages | 8 |

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

## Troubles

    failed: u8 kind, text sentence

`kind` is the number of a `tal::Kind`: missing 0, denied 1, exists 2, busy 3, full 4, refused 5,
broken 6, malformed 7, unsupported 8, overdue 9, other 10. The sentence is shown to people.

## What the program holds the process to

Everything the process says is checked as the words of a stranger. The process is killed, and every
question waiting on it fails as malformed, for: a frame longer than `ceiling` × 4 + 4096 bytes
(`media::ceiling` pixels) or shorter than 5; a frame of an unknown sort, or anything before the hello;
an answer to no question; a payload shorter or longer than its sort says; a number that is not
finite; a picture of another size than asked; more pages than the frame holds or a page of no area; a
read of more than 1 MiB; a trouble of an unknown kind.

A question unanswered within its limit (by default 10 s, navigation 30 s) kills the process and fails
as overdue. A process that ends fails every waiting question as broken; the next question starts it
anew, and the documents and views of the process that ended are lost with it. When the program lets
go of the adapter and of everything opened through it, the process is killed; it gets no farewell.

The adapter may also hold the process to a limit of memory (`Command::memory`): a job object on
Windows, `RLIMIT_DATA` on Linux and macOS.
