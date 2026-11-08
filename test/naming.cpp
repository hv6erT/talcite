#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/testing.hpp"

// Comments are the documentation of this code, so a name quoted in one with backticks has to
// exist. A comment that speaks of things that are gone looks true and is therefore worse than
// no comment at all.
//
// A name is checked against the document that defines it:
//
//   C++ name   - the code of the library, the tests and the demos, with comments left out
//   skin role  - a string in the code, since a role is named where it is given
//   file name  - the files of the project
//
// Dotted forms that are neither a role nor a file are expressions (`kept_.clear`, `seen.size`),
// not names: the library has no keyed documents to check them against.
//
// Comments are left out of the code that names are looked up in; otherwise every quoted name
// would prove itself. For the same reason this file's own code is left out too. A name from
// elsewhere (OpenSSL, Win32, POSIX, skin keys of Midnight Commander) or an idea not taken is
// written without backticks: why something is not here belongs to the sentence that says so, not
// to a list of exceptions in the test.
//
// A qualified name (`Page::hold`) is checked harder: it has to appear in the code as written, or
// its member has to appear in the body of the class, its own or inherited.
//
// Headers carry no comments at all, public or internal: what a name in a header does not say for
// itself is said next to its definition, in the `.cpp` file.
// The exception are headers that hold only inline or template code, listed in
// `inline-headers.txt`: with no `.cpp` file of their own, they explain their code where it is.

namespace {

namespace stood = std::filesystem;

// The project root, counted from this file: `naming.cpp` lies one directory below it.
stood::path root() { return stood::path{__FILE__}.parent_path().parent_path(); }

// The whole content of a file, or nothing when it cannot be read.
std::string slurped(const stood::path& where) {
    std::ifstream input{where, std::ios::binary};

    std::ostringstream held;

    held << input.rdbuf();

    return held.str();
}

// Whether the text ends with the given tail.
bool ends(std::string_view whole, std::string_view tail) {
    return whole.size() >= tail.size() &&
           whole.compare(whole.size() - tail.size(), tail.size(), tail) == 0;
}

// Whether the file is C++ code: a `.cpp` or `.hpp` file.
bool coded(const stood::path& one) {
    const std::string ending = one.extension().string();

    return ending == ".hpp" || ending == ".cpp";
}

// Whether the character can be part of an identifier.
bool wordy(char one) { return std::isalnum(static_cast<unsigned char>(one)) != 0 || one == '_'; }

struct Comment {
    std::size_t line = 0;
    std::string text;
};

struct Split {
    std::string code;
    std::vector<Comment> comments;
};

// A source file taken apart into code and comments. Comments become spaces in the code, so that
// line numbers stay; string and character literals, raw strings included, stay code, so that
// `https://` in a string is not a comment. An apostrophe right after a letter or digit is a digit
// separator (`1'000`), not the start of a character literal.
Split split(std::string_view source) {
    Split made;

    made.code.reserve(source.size());

    std::size_t line = 1;
    std::size_t at = 0;

    const auto keep = [&](char one) {
        if (one == '\n') ++line;

        made.code += one;
    };

    while (at < source.size()) {
        const char one = source[at];

        if (source.compare(at, 2, "//") == 0) {
            const std::size_t end = std::min(source.find('\n', at), source.size());

            made.comments.push_back({line, std::string{source.substr(at + 2, end - at - 2)}});
            made.code.append(end - at, ' ');
            at = end;

            continue;
        }

        if (source.compare(at, 2, "/*") == 0) {
            const std::size_t end = std::min(source.find("*/", at + 2), source.size());
            const std::size_t stop = std::min(end + 2, source.size());

            std::size_t from = at + 2;

            for (std::size_t here = from; here <= end && here < source.size(); ++here) {
                if (here == end || source[here] == '\n') {
                    made.comments.push_back({line, std::string{source.substr(from, here - from)}});

                    if (here < end) {
                        ++line;
                        from = here + 1;
                    }
                }
            }

            for (std::size_t here = at; here < stop; ++here) made.code += source[here] == '\n' ? '\n' : ' ';

            at = stop;

            continue;
        }

        const bool after_word = at > 0 && wordy(source[at - 1]);

        if (one == '"' && at > 0 && source[at - 1] == 'R' &&
            (at < 2 || !wordy(source[at - 2]) || std::string_view{"8LuU"}.find(source[at - 2]) != std::string_view::npos)) {
            const std::size_t open = source.find('(', at);

            if (open != std::string_view::npos) {
                const std::string closer = ")" + std::string{source.substr(at + 1, open - at - 1)} + "\"";
                const std::size_t end = std::min(source.find(closer, open), source.size());
                const std::size_t stop = std::min(end + closer.size(), source.size());

                for (std::size_t here = at; here < stop; ++here) keep(source[here]);

                at = stop;

                continue;
            }
        }

        if (one == '"' || (one == '\'' && !after_word)) {
            keep(one);
            ++at;

            while (at < source.size() && source[at] != one && source[at] != '\n') {
                if (source[at] == '\\' && at + 1 < source.size()) keep(source[at++]);

                keep(source[at++]);
            }

            if (at < source.size()) keep(source[at++]);

            continue;
        }

        keep(one);
        ++at;
    }

    return made;
}

// The code with the contents of string and character literals turned into spaces, so that a brace
// inside a literal does not count as a brace.
std::string blanked(std::string_view code) {
    std::string made{code};

    std::size_t at = 0;

    while (at < code.size()) {
        const char one = code[at];

        if (one == '"' && at > 0 && code[at - 1] == 'R' &&
            (at < 2 || !wordy(code[at - 2]) || std::string_view{"8LuU"}.find(code[at - 2]) != std::string_view::npos)) {
            const std::size_t open = code.find('(', at);

            if (open != std::string_view::npos) {
                const std::string closer = ")" + std::string{code.substr(at + 1, open - at - 1)} + "\"";
                const std::size_t end = std::min(code.find(closer, open), code.size());
                const std::size_t stop = std::min(end + closer.size(), code.size());

                for (std::size_t here = at + 1; here + 1 < stop; ++here)
                    if (made[here] != '\n') made[here] = ' ';

                at = stop;

                continue;
            }
        }

        if (one == '"' || (one == '\'' && !(at > 0 && wordy(code[at - 1])))) {
            std::size_t here = at + 1;

            while (here < code.size() && code[here] != one && code[here] != '\n') {
                if (code[here] == '\\' && here + 1 < code.size()) made[here++] = ' ';

                made[here++] = ' ';
            }

            at = here + 1;

            continue;
        }

        ++at;
    }

    return made;
}

// The text without spaces, tabs and line ends on either side.
std::string trimmed(std::string_view text) {
    const std::size_t from = text.find_first_not_of(" \t\r\n");

    if (from == std::string_view::npos) return {};

    return std::string{text.substr(from, text.find_last_not_of(" \t\r\n") - from + 1)};
}

// Leading access labels (`public:`) in front of a declaration, or nothing.
std::size_t labelled(std::string_view text) {
    for (const std::string_view label : {std::string_view{"public"}, std::string_view{"private"}, std::string_view{"protected"}}) {
        if (text.substr(0, label.size()) != label) continue;

        std::size_t at = label.size();

        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n')) ++at;

        if (at < text.size() && text[at] == ':' && (at + 1 == text.size() || text[at + 1] != ':')) return at + 1;
    }

    return 0;
}

enum class Head : std::uint8_t { none, function, initializing };

// What the text in front of an opening brace is. A function definition is a name with a parameter
// list, then only qualifiers (`const`, `noexcept`, `override`, a trailing return type) or a
// constructor's initializer list. A brace inside an initializer list that is not finished yet
// (`: size_{…}`, `: Dialog(Shape{…})`) is brace initialization of a member or a base. Control
// statements, lambdas, classes, namespaces and other brace initialization are none of these.
Head signature(std::string_view given) {
    std::string text;

    for (std::size_t at = 0; at < given.size();) {
        const std::size_t end = std::min(given.find('\n', at), given.size());
        const std::string line = trimmed(given.substr(at, end - at));

        if (!line.empty() && line.front() != '#') text += line + ' ';

        at = end + 1;
    }

    for (std::size_t at = text.find("[["); at != std::string::npos; at = text.find("[[")) {
        const std::size_t end = text.find("]]", at);

        if (end == std::string::npos) break;

        text.erase(at, end + 2 - at);
    }

    text = trimmed(text);

    while (const std::size_t skip = labelled(text)) text = trimmed(std::string_view{text}.substr(skip));

    std::size_t open = text.find('(');

    if (open == std::string::npos || open == 0) return Head::none;

    const std::string head = trimmed(std::string_view{text}.substr(0, open));

    // `operator()` has its parameter list after its own pair of parentheses.
    if (head.ends_with("operator") && text.compare(open, 2, "()") == 0) open = text.find('(', open + 2);

    if (open == std::string::npos) return Head::none;

    std::vector<std::string> words;
    std::string word;

    for (const char one : head + ' ') {
        if (wordy(one) && !(word.empty() && std::isdigit(static_cast<unsigned char>(one)) != 0)) {
            word += one;
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }

    constexpr std::string_view keys[] = {"if", "for", "while", "switch", "catch", "return", "else", "do", "sizeof",
                                         "alignof", "new", "delete", "throw", "case", "co_return", "static_assert",
                                         "typedef", "using", "class", "struct", "enum", "union", "namespace", "TRIAL",
                                         "extern"};

    const auto key = [&keys](const std::string& one) { return std::find(std::begin(keys), std::end(keys), one) != std::end(keys); };

    if (words.empty() || key(words.front()) || key(words.back())) return Head::none;
    if (words.front() == "template" && words.size() > 1 && key(words[1])) return Head::none;

    const bool operating = head.find("operator") != std::string::npos;

    if (!operating && head.find_first_of("=[]{}") != std::string::npos) return Head::none;
    if (!operating && !wordy(head.back())) return Head::none;

    std::size_t close = std::string::npos;
    std::size_t deep = 0;

    for (std::size_t at = open; at < text.size(); ++at) {
        if (text[at] == '(') ++deep;
        else if (text[at] == ')' && --deep == 0) {
            close = at;
            break;
        }
    }

    if (close == std::string::npos) return Head::none;

    std::string rest = trimmed(std::string_view{text}.substr(close + 1));

    while (!rest.empty()) {
        if (rest.starts_with("->") || rest.starts_with("requires")) return Head::function;

        if (rest.front() == ':' && !rest.starts_with("::")) {
            const std::string init = trimmed(std::string_view{rest}.substr(1));

            int level = 0;

            for (const char one : init) level += one == '(' ? 1 : one == ')' ? -1 : 0;

            const bool finished = level == 0 && !init.empty() && (init.back() == ')' || init.back() == '}');

            return finished ? Head::function : Head::initializing;
        }

        if (rest.starts_with("noexcept(")) {
            std::size_t level = 0;
            std::size_t at = 8;

            for (; at < rest.size(); ++at) {
                if (rest[at] == '(') ++level;
                else if (rest[at] == ')' && --level == 0) break;
            }

            rest = trimmed(std::string_view{rest}.substr(std::min(at + 1, rest.size())));

            continue;
        }

        bool taken = false;

        for (const std::string_view qualifier : {std::string_view{"const"}, std::string_view{"noexcept"}, std::string_view{"override"},
                                                 std::string_view{"final"}, std::string_view{"mutable"}}) {
            if (rest.starts_with(qualifier) && (rest.size() == qualifier.size() || !wordy(rest[qualifier.size()]))) {
                rest = trimmed(std::string_view{rest}.substr(qualifier.size()));
                taken = true;
                break;
            }
        }

        if (taken) continue;

        if (rest.front() == '&') {
            rest = trimmed(std::string_view{rest}.substr(rest.starts_with("&&") ? 2 : 1));
            continue;
        }

        return Head::none;
    }

    return Head::function;
}

struct Definition {
    std::size_t line = 0;
    bool empty = false;
};

// The function definitions of a source file: the line where each one starts (its `template` or
// attribute line, if any) and whether its body is empty.
std::vector<Definition> definitions(std::string_view source) {
    const std::string code = blanked(split(source).code);

    std::vector<std::size_t> lines(code.size() + 1, 1);

    for (std::size_t at = 0; at < code.size(); ++at) lines[at + 1] = lines[at] + (code[at] == '\n' ? 1 : 0);

    std::vector<Definition> made;
    std::size_t last = 0;

    for (std::size_t at = 0; at < code.size(); ++at) {
        const char one = code[at];

        if (one == ';' || one == '}') {
            last = at + 1;
            continue;
        }

        if (one != '{') continue;

        const std::string_view before = std::string_view{code}.substr(last, at - last);
        const bool parenthesized = std::count(before.begin(), before.end(), '(') > std::count(before.begin(), before.end(), ')');
        const Head head = parenthesized ? Head::initializing : signature(before);

        // Braces inside an initializer list or inside parentheses (a default argument `= {}`, a
        // lambda passed as an argument) are skipped, and the text before them goes on.
        if (head == Head::initializing) {
            std::size_t deep = 0;

            for (; at < code.size(); ++at) {
                if (code[at] == '{') ++deep;
                else if (code[at] == '}' && --deep == 0) break;
            }

            continue;
        }

        if (head == Head::function) {
            std::size_t from = last;

            for (;;) {
                while (from < at && std::isspace(static_cast<unsigned char>(code[from])) != 0) ++from;

                if (const std::size_t skip = labelled(std::string_view{code}.substr(from, at - from))) {
                    from += skip;
                    continue;
                }

                if (from < at && code[from] == '#') {
                    from = std::min(code.find('\n', from), at);
                    continue;
                }

                break;
            }

            std::size_t next = at + 1;

            while (next < code.size() && std::isspace(static_cast<unsigned char>(code[next])) != 0) ++next;

            made.push_back({lines[from], next < code.size() && code[next] == '}'});
        }

        last = at + 1;
    }

    return made;
}

// The lines that hold nothing but a comment.
std::set<std::size_t> commented(std::string_view source) {
    std::set<std::size_t> made;

    std::vector<std::string_view> rows;

    for (std::size_t at = 0; at <= source.size();) {
        const std::size_t end = std::min(source.find('\n', at), source.size());

        rows.push_back(source.substr(at, end - at));
        at = end + 1;
    }

    for (const Comment& one : split(source).comments) {
        if (one.line == 0 || one.line > rows.size()) continue;

        const std::string row = trimmed(rows[one.line - 1]);

        if (row.starts_with("//") || row.starts_with("/*") || row.starts_with("*")) made.insert(one.line);
    }

    return made;
}

// The code of the sources, without comments. This file's own code stays out (see the note at
// the top).
std::string gathered(const stood::path& from) {
    std::string whole;

    if (!stood::exists(from)) return whole;

    for (const auto& one : stood::recursive_directory_iterator{from})
        if (one.is_regular_file() && coded(one.path()) && one.path().filename() != "naming.cpp")
            whole += split(slurped(one.path())).code + '\n';

    return whole;
}

// A whole word, not a piece of another: `put` must not count because of `output`.
bool present(std::string_view whole, std::string_view stem) {
    if (stem.empty()) return false;

    for (std::size_t at = whole.find(stem); at != std::string_view::npos;
         at = whole.find(stem, at + 1)) {
        const bool before = at == 0 || !wordy(whole[at - 1]);
        const bool after = at + stem.size() == whole.size() || !wordy(whole[at + stem.size()]);

        if (before && after) return true;
    }

    return false;
}

// The body of a class or struct of the given name and the list of its bases. The word `class` or
// `struct` is looked for before the name, so that a use is not taken for a declaration, and
// forward declarations (`class Node;`) are skipped, as they have no body. An attribute between
// the word and the name (`class [[nodiscard]] Promise`) is stepped over.
struct Told {
    std::string body;
    std::vector<std::string> above;
};

// Every class or struct of the given name found in the code, with its body and bases.
std::vector<Told> told(std::string_view whole, std::string_view name) {
    std::vector<Told> every;

    for (std::size_t at = whole.find(name); at != std::string_view::npos;
         at = whole.find(name, at + 1)) {
        if (at + name.size() < whole.size() && wordy(whole[at + name.size()])) continue;

        std::size_t back = at;

        while (back > 0 && (whole[back - 1] == ' ' || whole[back - 1] == '\t')) --back;

        if (back >= 2 && whole.substr(back - 2, 2) == "]]") {
            const std::size_t open = whole.rfind("[[", back - 2);

            if (open != std::string_view::npos) {
                back = open;

                while (back > 0 && (whole[back - 1] == ' ' || whole[back - 1] == '\t')) --back;
            }
        }

        const bool klass = back >= 5 && whole.substr(back - 5, 5) == "class";
        const bool structure = back >= 6 && whole.substr(back - 6, 6) == "struct";

        if (!klass && !structure) continue;

        Told made;

        std::size_t here = at + name.size();
        std::string bases;

        while (here < whole.size() && whole[here] != '{' && whole[here] != ';') {
            bases += whole[here];
            ++here;
        }

        if (here == whole.size() || whole[here] == ';') continue;

        std::size_t deep = 0;
        const std::size_t from = here;

        for (; here < whole.size(); ++here) {
            if (whole[here] == '{') ++deep;
            else if (whole[here] == '}' && --deep == 0) break;
        }

        made.body = std::string{whole.substr(from, here - from)};

        std::string word;

        for (const char one : bases) {
            if (wordy(one)) {
                word += one;
            } else {
                if (word.size() > 2 && word != "public" && word != "private" &&
                    word != "protected" && word != "virtual")
                    made.above.push_back(word);

                word.clear();
            }
        }

        if (!word.empty() && word != "public" && word != "private" && word != "protected")
            made.above.push_back(word);

        every.push_back(std::move(made));
    }

    return every;
}

struct Named {
    std::set<std::string> files;
    std::string code;
    std::string build;
};

constexpr const char* bases[] = {"talcite", "test", "demo"};

// Everything names are checked against, gathered once per test.
Named taken() {
    Named made;

    const stood::path here = root();

    for (const char* base : bases) made.code += gathered(here / base);

    for (const char* one : {"CMakeLists.txt", "CMakePresets.json", "talcite/CMakeLists.txt",
                            "test/CMakeLists.txt", "demo/CMakeLists.txt"})
        made.build += slurped(here / one) + '\n';

    for (const auto& one : stood::directory_iterator{here})
        if (one.is_regular_file()) made.files.insert(one.path().filename().string());

    for (const char* base : bases)
        for (const auto& one : stood::recursive_directory_iterator{here / base})
            if (one.is_regular_file()) made.files.insert(one.path().filename().string());

    return made;
}

// A qualified name either appears in the code as written, or its member lies in the body of the
// class, its own or inherited: `Select::options` comes from `OptionsMixin`.
bool inherited(const Named& all, const std::string& owner, const std::string& stem, int deep) {
    if (deep > 4) return false;

    for (const Told& one : told(all.code, owner)) {
        if (present(one.body, stem)) return true;

        for (const std::string& up : one.above)
            if (inherited(all, up, stem, deep + 1)) return true;
    }

    return false;
}

// Whether the text is a single C++ identifier.
bool identifier(std::string_view one) {
    if (one.empty() || (!std::isalpha(static_cast<unsigned char>(one[0])) && one[0] != '_'))
        return false;

    return std::all_of(one.begin(), one.end(), wordy);
}

// `Node` or `Page::hold`; `name()` counts the same as `name`.
bool cplusplus(std::string_view one, std::string& owner, std::string& stem) {
    const std::size_t colon = one.find("::");

    if (colon == std::string_view::npos) {
        if (!identifier(one)) return false;

        owner.clear();
        stem = std::string{one};

        return true;
    }

    if (one.find("::", colon + 2) != std::string_view::npos) return false;

    if (!identifier(one.substr(0, colon)) || !identifier(one.substr(colon + 2))) return false;

    owner = std::string{one.substr(0, colon)};
    stem = std::string{one.substr(colon + 2)};

    return true;
}

// `talcite.frame`, `node.cpp`: parts separated by dots, without the characters that would make a
// pattern (`talcite.panel.name.*`) or a piece of code out of it.
bool dotted(std::string_view one) {
    if (one.empty() || one.find('.') == std::string_view::npos) return false;

    bool fresh = true;

    for (const char letter : one) {
        if (letter == '.') {
            if (fresh) return false;

            fresh = true;

            continue;
        }

        if (!wordy(letter)) return false;

        fresh = false;
    }

    return !fresh;
}

// Whether the dotted text names a file, judged by its extension.
bool filed(std::string_view one) {
    for (const char* tail : {".cpp", ".hpp", ".json", ".md", ".txt", ".ini", ".c", ".h"})
        if (ends(one, tail)) return true;

    return false;
}

// Headers that hold only inline or template code have no `.cpp` file to explain it in, so they
// keep their comments. They are listed, one path per line, in `inline-headers.txt`; their
// comments are checked like those of a `.cpp` file.
std::set<std::string> inlined() {
    std::set<std::string> made;

    std::istringstream lines{slurped(root() / "test" / "inline-headers.txt")};
    std::string line;

    while (std::getline(lines, line))
        if (!line.empty()) made.insert(line);

    return made;
}

// Every code file of the project, by its path relative to the root, with `/` as the separator.
std::vector<std::pair<std::string, stood::path>> sources() {
    std::vector<std::pair<std::string, stood::path>> made;

    for (const char* base : bases)
        for (const auto& one : stood::recursive_directory_iterator{root() / base})
            if (one.is_regular_file() && coded(one.path()))
                made.emplace_back(one.path().lexically_relative(root()).generic_string(), one.path());

    std::sort(made.begin(), made.end());

    return made;
}

// What is wrong with one name quoted in a comment, or nothing.
std::string checked(const Named& all, std::string one) {
    if (one.size() > 2 && one.compare(one.size() - 2, 2, "()") == 0) one.resize(one.size() - 2);

    std::string owner;
    std::string stem;

    if (dotted(one)) {
        if (filed(one)) return all.files.contains(one) ? "" : "no such file";

        if (one.rfind("talcite.", 0) == 0)
            return present(all.code, '"' + one + '"') ? "" : "no such role in the code";

        return "";
    }

    if (!cplusplus(one, owner, stem)) return "";

    if (!owner.empty()) {
        if (present(all.code, one) || inherited(all, owner, stem, 0)) return "";

        return "not in the code, nor in the body of " + owner;
    }

    if (stem.size() < 3) return "";

    if (present(all.code, stem) || present(all.build, stem)) return "";

    return "not in the code of the library";
}

}

TRIAL("Names: every name quoted in a comment exists") {
    const Named all = taken();
    const std::set<std::string> kept = inlined();

    std::vector<std::string> bad;

    for (const auto& [name, where] : sources()) {
        if (where.extension() != ".cpp" && !kept.contains(name)) continue;

        for (const Comment& comment : split(slurped(where)).comments) {
            const std::string& text = comment.text;

            for (std::size_t at = text.find('`'); at != std::string::npos; at = text.find('`', at + 1)) {
                const std::size_t upto = text.find('`', at + 1);

                if (upto == std::string::npos) break;

                const std::string one = text.substr(at + 1, upto - at - 1);
                const std::string wrong = checked(all, one);

                if (!wrong.empty())
                    bad.push_back("  " + name + ":" + std::to_string(comment.line) + ": " + one + " - " + wrong);

                at = upto;
            }
        }
    }

    if (!bad.empty()) {
        std::string said = "names quoted in comments that do not exist:";

        for (const std::string& one : bad) said += '\n' + one;

        said += "\n\n  A name from elsewhere, or one that is gone, is written without backticks.";

        ::tal::testing::fell(__FILE__, __LINE__, said);
    }
}

TRIAL("Names: headers carry no comments") {
    const std::set<std::string> kept = inlined();

    std::vector<std::string> bad;

    for (const auto& [name, where] : sources()) {
        if (where.extension() != ".hpp" || kept.contains(name)) continue;

        for (const Comment& comment : split(slurped(where)).comments)
            bad.push_back("  " + name + ":" + std::to_string(comment.line));
    }

    if (!bad.empty()) {
        std::string said = "comments in headers - they belong next to the definition, in the .cpp file:";

        for (const std::string& one : bad) said += '\n' + one;

        ::tal::testing::fell(__FILE__, __LINE__, said);
    }
}

// Every function that does something says so in a comment right above it. A defaulted or empty
// function (`= default`, `{}`) needs none, and neither do lambdas, which the comment of their
// enclosing function covers, nor trials, whose name is their description.
TRIAL("Names: every function that does something has a comment above it") {
    const std::set<std::string> kept = inlined();

    std::vector<std::string> bad;

    for (const auto& [name, where] : sources()) {
        if (where.extension() != ".cpp" && !kept.contains(name)) continue;

        const std::string source = slurped(where);
        const std::set<std::size_t> above = commented(source);

        for (const Definition& one : definitions(source))
            if (!one.empty && !above.contains(one.line - 1)) bad.push_back("  " + name + ":" + std::to_string(one.line));
    }

    if (!bad.empty()) {
        std::string said = "functions without a comment right above them:";

        for (const std::string& one : bad) said += '\n' + one;

        ::tal::testing::fell(__FILE__, __LINE__, said);
    }
}

// The list of inline headers names headers that exist, so that a header removed or renamed does
// not leave an exception behind.
TRIAL("Names: the inline headers on the list exist") {
    const std::set<std::string> listed = inlined();

    std::set<std::string> found;

    for (const auto& [name, where] : sources())
        if (where.extension() == ".hpp") found.insert(name);

    std::vector<std::string> bad;

    for (const std::string& one : listed)
        if (!found.contains(one)) bad.push_back("  " + one);

    if (!bad.empty()) {
        std::string said = "headers in inline-headers.txt that are not in the project:";

        for (const std::string& one : bad) said += '\n' + one;

        ::tal::testing::fell(__FILE__, __LINE__, said);
    }
}

TRIAL("Names: function definitions are told apart from statements, lambdas and classes") {
    const std::vector<Definition> found = definitions(
        "namespace x {\n"                                        // 1
        "struct Held : Base {\n"                                 // 2
        "public:\n"                                              // 3
        "    explicit Held(int one) : one_(one) {}\n"            // 4
        "    int one() const noexcept { return one_; }\n"        // 5
        "};\n"                                                   // 6
        "template <typename T>\n"                                // 7
        "[[nodiscard]] auto twice(T one) -> T {\n"               // 8
        "    if (one > 0) { return one; }\n"                     // 9
        "    const auto f = [&](int a) { return a; };\n"         // 10
        "    std::for_each(a, b, [](int c) { (void)c; });\n"     // 11
        "    for (int i = 0; i < 3; ++i) {\n"                    // 12
        "    }\n"                                                // 13
        "    return \"{\" + one;\n"                             // 14
        "}\n"                                                    // 15
        "bool operator==(const Held& a, const Held& b) {\n"      // 16
        "    return a.one() == b.one();\n"                       // 17
        "}\n"                                                    // 18
        "TRIAL(\"name\") {\n"                                   // 19
        "}\n"                                                    // 20
        "}\n");

    SAME(found.size(), std::size_t{4});

    if (found.size() == 4) {
        SAME(found[0].line, std::size_t{4});
        OUGHT(found[0].empty);
        SAME(found[1].line, std::size_t{5});
        SAME(found[2].line, std::size_t{7});
        SAME(found[3].line, std::size_t{16});
        OUGHT(!found[3].empty);
    }

    // Brace initialization in a constructor's initializer list is not a body.
    const std::vector<Definition> built = definitions("A::A(int x)\n    : b_{x}, c_(Shape{x}) {\n    go();\n}\n");

    SAME(built.size(), std::size_t{1});

    if (built.size() == 1) {
        SAME(built[0].line, std::size_t{1});
        OUGHT(!built[0].empty);
    }

    // Neither are braces in a default argument.
    const std::vector<Definition> defaulted = definitions("void f(int a, Mods b = {}) {\n    go();\n}\n");

    SAME(defaulted.size(), std::size_t{1});
}

TRIAL("Names: the split keeps strings and raw strings as code") {
    const Split made = split("int a = 1'000; // one\n"
                             "const char* b = \"https://x\"; /* two\nthree */ char c = '/';\n"
                             "const char* d = R\"j(// not a comment)j\";\n");

    SAME(made.comments.size(), std::size_t{3});

    if (made.comments.size() == 3) {
        SAME(made.comments[0].line, std::size_t{1});
        SAME(made.comments[0].text, std::string{" one"});
        SAME(made.comments[1].line, std::size_t{2});
        SAME(made.comments[1].text, std::string{" two"});
        SAME(made.comments[2].line, std::size_t{3});
        SAME(made.comments[2].text, std::string{"three "});
    }

    OUGHT(made.code.find("https://x") != std::string::npos);
    OUGHT(made.code.find("// not a comment") != std::string::npos);
    OUGHT(made.code.find("one") == std::string::npos);
    SAME(std::count(made.code.begin(), made.code.end(), '\n'), std::ptrdiff_t{4});
}

namespace {

// What a preprocessor condition is on Windows: true, false, or not known (nothing). Only the names
// the shared code of `fs` asks about are known: `_WIN32` is defined there, the POSIX file system
// paths (`TALCITE_LINUX_FS`, `TALCITE_BSD_FS`) and the POSIX systems are not. A condition of several
// parts joined by `&&` or `||` is worked out from its parts.
std::optional<bool> windowed(std::string_view condition) {
    const auto trimmed = [](std::string_view one) {
        while (!one.empty() && std::isspace(static_cast<unsigned char>(one.front())) != 0) one.remove_prefix(1);
        while (!one.empty() && std::isspace(static_cast<unsigned char>(one.back())) != 0) one.remove_suffix(1);

        return one;
    };

    condition = trimmed(condition);

    for (const std::string_view joint : {"||", "&&"}) {
        const std::size_t at = condition.find(joint);

        if (at == std::string_view::npos) continue;

        const std::optional<bool> left = windowed(condition.substr(0, at));
        const std::optional<bool> right = windowed(condition.substr(at + 2));

        if (joint == "&&") {
            if (left == false || right == false) return false;
            if (left == true && right == true) return true;
        } else {
            if (left == true || right == true) return true;
            if (left == false && right == false) return false;
        }

        return std::nullopt;
    }

    const bool negated = !condition.empty() && condition.front() == '!';

    if (negated) condition = trimmed(condition.substr(1));

    if (condition.rfind("defined", 0) == 0) condition = trimmed(condition.substr(7));

    while (!condition.empty() && condition.front() == '(' && condition.back() == ')')
        condition = trimmed(condition.substr(1, condition.size() - 2));

    std::optional<bool> made;

    if (condition == "_WIN32") made = true;

    for (const char* posix : {"TALCITE_LINUX_FS", "TALCITE_BSD_FS", "__linux__", "__APPLE__", "__FreeBSD__"})
        if (condition == posix) made = false;

    if (made && negated) made = !*made;

    return made;
}

// For every line of the code, whether it can be compiled on Windows, from the `#if` directives
// around it. A branch is out when its condition is false there, or when an earlier branch of the
// same `#if` is surely taken; a condition that is not known keeps the branch in.
std::vector<bool> on_windows(const std::string& code) {
    struct Branch {
        bool outer = true;
        bool taken = false;
        bool live = true;
    };

    std::vector<Branch> open;
    std::vector<bool> made{true};

    std::istringstream lines{code};
    std::string line;

    while (std::getline(lines, line)) {
        const std::size_t first = line.find_first_not_of(" \t");
        const bool live = open.empty() || open.back().live;

        if (first == std::string::npos || line[first] != '#') {
            made.push_back(live);

            continue;
        }

        std::string_view directive = std::string_view{line}.substr(first + 1);

        while (!directive.empty() && directive.front() == ' ') directive.remove_prefix(1);

        const auto opened = [&open, live](std::optional<bool> condition) {
            open.push_back(Branch{live, condition == true, live && condition != false});
        };

        if (directive.rfind("ifdef", 0) == 0) opened(windowed("defined(" + std::string{directive.substr(5)} + ")"));
        else if (directive.rfind("ifndef", 0) == 0) opened(windowed("!defined(" + std::string{directive.substr(6)} + ")"));
        else if (directive.rfind("if", 0) == 0) opened(windowed(directive.substr(2)));
        else if (!open.empty() && directive.rfind("elif", 0) == 0) {
            const std::optional<bool> condition = windowed(directive.substr(4));
            Branch& one = open.back();

            one.live = one.outer && !one.taken && condition != false;
            one.taken = one.taken || condition == true;
        } else if (!open.empty() && directive.rfind("else", 0) == 0) {
            Branch& one = open.back();

            one.live = one.outer && !one.taken;
            one.taken = true;
        } else if (!open.empty() && directive.rfind("endif", 0) == 0) {
            open.pop_back();
        }

        made.push_back(open.empty() || open.back().live);
    }

    return made;
}

// The arguments of a call whose opening bracket is at `at`, split at the commas of the top level,
// and where the call ends.
std::vector<std::string> arguments(const std::string& code, std::size_t at, std::size_t& end) {
    std::vector<std::string> made{std::string{}};

    int depth = 0;

    for (end = at; end < code.size(); ++end) {
        const char one = code[end];

        if (one == '(' || one == '{' || one == '[') {
            if (depth++ > 0) made.back() += one;

            continue;
        }

        if (one == ')' || one == '}' || one == ']') {
            if (--depth == 0) break;

            made.back() += one;

            continue;
        }

        if (one == ',' && depth == 1) made.emplace_back();
        else made.back() += one;
    }

    return made;
}

// Which arguments of a `std::filesystem` call are paths that reach the system, by the function's
// name and the number of its arguments; nothing for a call that does not reach it (a status asked
// about, a path taken apart). A symlink's target is text stored in the link, not a path the system
// opens, so only the link's own path counts.
std::vector<std::size_t> reaching(std::string_view name, std::size_t many) {
    for (const char* one : {"exists", "is_directory", "is_regular_file", "is_symlink"})
        if (name == one) return many == 2 ? std::vector<std::size_t>{0} : std::vector<std::size_t>{};

    for (const char* one : {"equivalent", "rename", "create_hard_link", "copy", "copy_file"})
        if (name == one) return {0, 1};

    for (const char* one : {"create_symlink", "create_directory_symlink"})
        if (name == one) return {1};

    for (const char* one : {"status", "symlink_status", "file_size", "read_symlink", "space", "canonical", "weakly_canonical",
                            "remove", "remove_all", "create_directory", "create_directories", "permissions", "is_empty",
                            "hard_link_count", "last_write_time", "resize_file", "directory_iterator",
                            "recursive_directory_iterator"})
        if (name == one) return {0};

    return {};
}

}

// The shared code of `fs` hands every path to `std::filesystem` through `long_form`, so that on
// Windows a path beyond 260 characters works whatever the manifest and the registry say. A call that
// reaches the system with a path given as it is passes on Linux and on a short path, and breaks only
// deep in a real tree on Windows; this trial finds it where it is written. Code that is never
// compiled on Windows (the POSIX branches) is left out, and so are calls that do not reach the
// system.
TRIAL("Names: on Windows the shared fs code gives std::filesystem every path through long_form") {
    std::vector<std::string> bad;

    for (const char* name : {"copy.cpp", "file.cpp", "fs.cpp", "http.cpp", "watch.cpp"}) {
        const std::string code = split(slurped(root() / "talcite" / "src" / name)).code;
        const std::vector<bool> live = on_windows(code);

        for (const std::string_view spelled : {"stood::", "std::filesystem::"}) {
            std::size_t line = 1;
            std::size_t counted = 0;

            for (std::size_t at = code.find(spelled); at != std::string::npos; at = code.find(spelled, at + 1)) {
                line += static_cast<std::size_t>(std::count(code.begin() + static_cast<std::ptrdiff_t>(counted),
                                                            code.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
                counted = at;

                std::size_t upto = at + spelled.size();

                while (upto < code.size() && wordy(code[upto])) ++upto;

                const std::string called = code.substr(at + spelled.size(), upto - at - spelled.size());

                while (upto < code.size() && code[upto] == ' ') ++upto;

                // An object made on the spot (`stood::directory_iterator at{...}`) is a call too.
                if (upto < code.size() && wordy(code[upto])) {
                    while (upto < code.size() && wordy(code[upto])) ++upto;

                    while (upto < code.size() && code[upto] == ' ') ++upto;
                }

                if (upto >= code.size() || (code[upto] != '(' && code[upto] != '{')) continue;

                if (line >= live.size() || !live[line]) continue;

                std::size_t end = 0;

                const std::vector<std::string> given = arguments(code, upto, end);

                for (const std::size_t which : reaching(called, given.size())) {
                    std::string_view one = which < given.size() ? std::string_view{given[which]} : std::string_view{};

                    while (!one.empty() && one.front() == ' ') one.remove_prefix(1);

                    if (one.rfind("long_form(", 0) != 0 && one.rfind("fs::core::long_form(", 0) != 0)
                        bad.push_back("  " + std::string{name} + ":" + std::to_string(line) + ": " + std::string{spelled} + called);
                }
            }
        }
    }

    if (!bad.empty()) {
        std::string said = "std::filesystem calls that reach the system with a path not given through long_form:";

        for (const std::string& one : bad) said += '\n' + one;

        ::tal::testing::fell(__FILE__, __LINE__, said);
    }
}

// The reading of `#if` directives keeps a branch in unless it surely is not compiled on Windows.
TRIAL("Names: the Windows branches are told from the POSIX ones") {
    const std::vector<bool> live = on_windows("a\n"
                                              "#ifdef _WIN32\n"
                                              "b\n"
                                              "#else\n"
                                              "c\n"
                                              "#endif\n"
                                              "#if defined(TALCITE_LINUX_FS)\n"
                                              "d\n"
                                              "#elif defined(SOMETHING)\n"
                                              "e\n"
                                              "#else\n"
                                              "f\n"
                                              "#endif\n"
                                              "#if !defined(_WIN32) && !defined(TALCITE_GENERIC_POSIX)\n"
                                              "g\n"
                                              "#endif\n");

    SAME(live.size(), std::size_t{17});

    if (live.size() == 17) {
        OUGHT(live[1] && live[3] && !live[5]);
        OUGHT(!live[8] && live[10] && live[12]);
        OUGHT(!live[15]);
    }
}
