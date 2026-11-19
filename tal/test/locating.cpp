#include <optional>
#include <string>
#include <vector>

#include "tal/path.hpp"

#include "locating.hpp"
#include "tal/testing.hpp"

// Files named in text: file URIs both ways, and the bytes terminals paste when files are dropped
// on their windows, read as a shell would read them and checked against the paths they name.

namespace {

using tal::Path;
using Words = std::optional<std::vector<std::string>>;
using Paths = std::optional<std::vector<Path>>;

// The paths of the texts.
Paths paths(std::vector<std::string> each) {
    std::vector<Path> made;

    for (const std::string& one : each) made.emplace_back(one);

    return made;
}

}

// A path becomes a file URI with every byte but the unreserved ones escaped, and back; a URI of
// another host or scheme, a broken escape or an escaped NUL names no file.
TRIAL("Locating: file URIs") {
#ifndef _WIN32
    SAME(tal::locating::located(Path{"/home/ala/My File (1).txt"}), std::string{"file:///home/ala/My%20File%20%281%29.txt"});
    SAME(tal::locating::located(Path{"/tmp/żółw~-_."}), std::string{"file:///tmp/%C5%BC%C3%B3%C5%82w~-_."});
    OUGHT(tal::locating::unlocated("file:///tmp/%C5%BC%c3%b3%C5%82w") == Path{"/tmp/żółw"});
    OUGHT(tal::locating::unlocated("file://localhost/a%20b") == Path{"/a b"});
#endif
    OUGHT(!tal::locating::unlocated("file://example.org/a").has_value());
    OUGHT(!tal::locating::unlocated("http:///a").has_value());
    OUGHT(!tal::locating::unlocated("file://").has_value());
    OUGHT(!tal::locating::unlocated("file:///a%2").has_value());
    OUGHT(!tal::locating::unlocated("file:///a%zz").has_value());
    OUGHT(!tal::locating::unlocated("file:///a%00b").has_value());
    OUGHT(tal::locating::listed("# comment\r\nhttp://x/y\r\n\r\n").empty());
}

// The words of a shell: quotes of both kinds, pieces joined, backslashes before signs and before a
// line break; on Windows a backslash is only a separator of folders.
TRIAL("Locating: words as a shell parts them") {
    SAME(tal::locating::words("", false), Words{std::vector<std::string>{}});
    SAME(tal::locating::words(" \t\r\n", false), Words{std::vector<std::string>{}});
    SAME(tal::locating::words("a  b\tc\r\nd ", false), (Words{{"a", "b", "c", "d"}}));
    SAME(tal::locating::words("'a b' 'it'\\''s' ''", false), (Words{{"a b", "it's", ""}}));
    SAME(tal::locating::words(R"("a\"b\\c\$d\`e\f" x"y"z)", false), (Words{{"a\"b\\c$d`e\\f", "xyz"}}));
    SAME(tal::locating::words(R"(My\ File\(1\).txt \\x)", false), (Words{{"My File(1).txt", "\\x"}}));
    SAME(tal::locating::words("/a\\\nb ''\\\n c", false), (Words{{"/ab", "", "c"}}));
    SAME(tal::locating::words(R"("C:\Users\Ala\My File.txt" C:\Users\Ala\b.png \\server\share\c)", true),
         (Words{{"C:\\Users\\Ala\\My File.txt", "C:\\Users\\Ala\\b.png", "\\\\server\\share\\c"}}));
    SAME(tal::locating::words(R"("C:\" C:\M\Don't.mp3 C:\M\Can't.mp3)", true), (Words{{"C:\\", "C:\\M\\Don't.mp3", "C:\\M\\Can't.mp3"}}));
    SAME(tal::locating::words("/a\\\r\nb", false), (Words{{"/ab"}}));
    SAME(tal::locating::words("/a\\\rb c", false), (Words{{"/ab", "c"}}));
    SAME(tal::locating::words(" \\\n x", false), (Words{{"x"}}));

    OUGHT(!tal::locating::words("'open", false).has_value());
    OUGHT(!tal::locating::words("\"open", false).has_value());
    OUGHT(!tal::locating::words("\"open\\\"", false).has_value());
    OUGHT(!tal::locating::words("end\\", false).has_value());
    OUGHT(!tal::locating::words("a\x1b[201~", false).has_value());
    OUGHT(!tal::locating::words("'a\x07'", false).has_value());
    OUGHT(!tal::locating::words("\"a\x7f\"", true).has_value());
    OUGHT(!tal::locating::words("\\\x01", false).has_value());
    OUGHT(!tal::locating::words(std::string{"a\0b", 3}, false).has_value());
    OUGHT(!tal::locating::words("'/a/Don\n/a/Can'", false).has_value());
    OUGHT(!tal::locating::words("\"/a\r\n/b\"", false).has_value());
}

#ifndef _WIN32

// What GNOME Terminal, Konsole, kitty, iTerm2 and terminals that paste URIs or a path to a line
// write for dropped files, read as the files; text that names anything else is no drop.
TRIAL("Locating: dropped files as terminals paste them") {
    SAME(tal::locating::dropped("'/home/ala/My File.txt' '/home/ala/b.png' "), (paths({"/home/ala/My File.txt", "/home/ala/b.png"})));
    SAME(tal::locating::dropped("/home/ala/a.txt "), (paths({"/home/ala/a.txt"})));
    SAME(tal::locating::dropped("'/home/ala/it'\\''s.txt'"), (paths({"/home/ala/it's.txt"})));
    SAME(tal::locating::dropped(R"(/Users/ala/My\ File.txt /Users/ala/b\(1\).png )"), (paths({"/Users/ala/My File.txt", "/Users/ala/b(1).png"})));
    SAME(tal::locating::dropped("file:///home/ala/My%20File.txt\r\nfile:///home/ala/b.png\r\n"),
         (paths({"/home/ala/My File.txt", "/home/ala/b.png"})));
    SAME(tal::locating::dropped("/home/ala/My File.txt\n/home/ala/b.png\n"), (paths({"/home/ala/My File.txt", "/home/ala/b.png"})));
    SAME(tal::locating::dropped("/etc/passwd and more"), (paths({"/etc/passwd and more"})));
    SAME(tal::locating::dropped("/home/a/Don't.txt\n/home/a/Can't.txt\n"), (paths({"/home/a/Don't.txt", "/home/a/Can't.txt"})));
    SAME(tal::locating::dropped("/home/ala/My File.txt "), (paths({"/home/ala/My File.txt"})));

    OUGHT(!tal::locating::dropped("").has_value());
    OUGHT(!tal::locating::dropped(" \n").has_value());
    OUGHT(!tal::locating::dropped("hello world").has_value());
    OUGHT(!tal::locating::dropped("ls -l /tmp").has_value());
    OUGHT(!tal::locating::dropped("'/tmp/open").has_value());
    OUGHT(!tal::locating::dropped("/tmp/a\x1b").has_value());
    OUGHT(!tal::locating::dropped("file://example.org/a").has_value());
    OUGHT(!tal::locating::dropped("/tmp/a\nrelative").has_value());
}

#else

// What Windows Terminal writes for dropped files, read as the files; text that names anything else
// is no drop, and neither is a share of another machine, which is never asked about.
TRIAL("Locating: dropped files as Windows Terminal pastes them") {
    SAME(tal::locating::dropped(R"("C:\Users\Ala\My File.txt" C:\Users\Ala\b.png )"), (paths({"C:\\Users\\Ala\\My File.txt", "C:\\Users\\Ala\\b.png"})));
    OUGHT(!tal::locating::dropped(R"(\\server\share\c.txt)").has_value());
    OUGHT(!tal::locating::dropped("\"C:\\a.txt\" \\\\server\\share\\c.txt").has_value());

    OUGHT(!tal::locating::dropped("hello world").has_value());
    OUGHT(!tal::locating::dropped("dir C:\\").has_value());
}

#endif
