#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "faking.hpp"
#include "talcite/bytes.hpp"
#include "talcite/encoding.hpp"
#include "talcite/following.hpp"
#include "talcite/launcher.hpp"
#include "talcite/link.hpp"
#include "talcite/loop.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"
#include "talcite/viewer.hpp"

using namespace talcite;

// A link and the launcher behind it. The link is tried with a fake launcher that writes down what
// it was asked to open and answers as told, so no browser opens during trials; the system launcher
// is tried on the paths it decides without starting anything: over ssh, for an address it does not
// open, without a session, for an address that is no address.

namespace {

// A launcher that opens nothing: it keeps every address it was asked about and answers `answer`.
class Opening : public Launcher {
public:
    std::vector<std::string> asked;
    Launched answer = Launched::opened;

    // The launcher's name.
    [[nodiscard]] std::string_view name() const override { return "fake launcher"; }

    // Keeps the address and answers at once.
    [[nodiscard]] Promise<Launched> open(std::string address) override {
        asked.push_back(std::move(address));

        return ready(answer);
    }
};

// A link to `address` with `text`, behind the fake launcher `with`.
std::shared_ptr<ui::Link> linked(std::string text, std::string address, const std::shared_ptr<Opening>& with) {
    return std::make_shared<ui::Link>(std::move(text), std::move(address), Seam<Launcher>{with});
}

// What the launcher behind the console and the clipboard answers about the address.
Launched answered(const std::shared_ptr<faking::Terminal>& line, const std::shared_ptr<faking::Clipboard>& board, std::string address) {
    return launcher(Seam<Console>{line}, Seam<Clipboard>{board}).provider().open(std::move(address)).get();
}

// Turns the loop until `done` says so, waiting on its bell between turns, for at most two seconds:
// an answer reaches the loop from elsewhere and may come a moment after it was given.
template <typename Done>
bool awaited(Loop& loop, Done done) {
    for (int at = 0; at < 200; ++at) {
        (void)loop.turn();

        if (done()) return true;

        (void)loop.bell().wait(std::chrono::milliseconds{10});
    }

    return false;
}

// Turns the loop for a while, so that whatever would come has had its time to come.
void settled(Loop& loop) {
    for (int at = 0; at < 5; ++at) {
        (void)loop.turn();
        (void)loop.bell().wait(std::chrono::milliseconds{10});
    }
}

}

// --- Drawing -------------------------------------------------------------------------------------

// A link shows its text underlined (the built-in look of `talcite.link`), with the hyperlink over
// exactly its cells; the rest of its rectangle is not drawn.
TRIAL("Link: shows its text underlined, with the hyperlink over the text") {
    ui::Root top;

    const auto opening = std::make_shared<Opening>();
    const auto one = linked("strona", "https://example.org/", opening);

    top.children().add(one);

    const Snapshot made = top.render({{0, 0}, {10, 1}});

    ought(made.at({0, 0}).style.under);
    same(made.at({0, 0}).letter, char32_t{U's'});
    ought(made.at({5, 0}).link != 0);
    same(made.at({6, 0}).link, std::uint32_t{0});
    ought(!made.at({6, 0}).style.under);
    same(made.links().size(), std::size_t{1});
    same(made.links().front().address, std::string{"https://example.org/"});
    same(one->wants({20, 3}).wide, 6);
    same(one->wants({20, 3}).high, 1);
}

// Without text it shows the address; too narrow, it is cut with the ellipsis, and the link covers
// what is shown.
TRIAL("Link: without text it shows the address, cut to the room it has") {
    ui::Root top;

    const auto one = linked("", "https://example.org/", std::make_shared<Opening>());

    top.children().add(one);

    const Snapshot made = top.render({{0, 0}, {6, 1}});

    same(made.at({0, 0}).letter, char32_t{U'h'});
    same(made.at({5, 0}).letter, char32_t{U'…'});
    ought(made.at({5, 0}).link != 0);
}

// A disabled link is faint and carries no hyperlink, so the terminal does not open it either; in
// focus a link is reversed and stays underlined.
TRIAL("Link: disabled it is faint without a hyperlink, in focus reversed and underlined") {
    ui::Root top;

    const auto one = linked("strona", "https://example.org/", std::make_shared<Opening>());

    top.children().add(one);

    one->focus();

    const Snapshot focused = top.render({{0, 0}, {10, 1}});

    ought(focused.at({0, 0}).style.reverse);
    ought(focused.at({0, 0}).style.under);

    one->enabled(false);

    const Snapshot off = top.render({{0, 0}, {10, 1}});

    ought(off.at({0, 0}).style.faint);
    same(off.at({0, 0}).link, std::uint32_t{0});
}

// A new address is a new hyperlink: under the old mark the frames would not differ and the terminal
// would keep the old address.
TRIAL("Link: a new address takes a new hyperlink") {
    ui::Root top;

    const auto one = linked("strona", "https://example.org/one", std::make_shared<Opening>());

    top.children().add(one);

    const std::uint32_t before = top.render({{0, 0}, {10, 1}}).at({0, 0}).link;

    one->address("https://example.org/two");

    const Snapshot after = top.render({{0, 0}, {10, 1}});

    ought(after.at({0, 0}).link != before);
    same(one->address(), std::string{"https://example.org/two"});
    same(after.links().front().address, std::string{"https://example.org/two"});
}

// --- Following -----------------------------------------------------------------------------------

// Enter follows the link in focus: the launcher gets the address, and its answer comes back as
// `FollowedEvent` on the loop.
TRIAL("Link: Enter follows it through the launcher, and the answer comes as FollowedEvent") {
    Loop loop;
    ui::Root top;

    const auto opening = std::make_shared<Opening>();

    opening->answer = Launched::copied;

    const auto one = linked("strona", "https://example.org/", opening);

    top.children().add(one);

    one->focus();

    std::optional<Launched> told;

    const ui::Hook heard = one->on<ui::FollowedEvent>([&told](ui::FollowedEvent& what) {
        told = what.how();

        same(what.address(), std::string{"https://example.org/"});

        return true;
    });

    ui::KeyEvent enter{ui::Key{ui::Code::enter}};

    ought(top.dispatch(enter));
    ought(awaited(loop, [&told] { return told.has_value(); }));

    same(opening->asked, (std::vector<std::string>{"https://example.org/"}));
    same(static_cast<int>(told.value_or(Launched::refused)), static_cast<int>(Launched::copied));
}

// `FollowEvent` goes first, and whoever takes it takes the link: the launcher is not asked.
TRIAL("Link: a FollowEvent taken keeps the launcher out") {
    Loop loop;
    ui::Root top;

    const auto opening = std::make_shared<Opening>();
    const auto one = linked("strona", "https://example.org/", opening);

    top.children().add(one);

    std::string taken;

    const ui::Hook heard = top.on<ui::FollowEvent>([&taken](ui::FollowEvent& what) {
        taken = what.address();

        return true;
    });

    one->follow();

    settled(loop);

    same(taken, std::string{"https://example.org/"});
    ought(opening->asked.empty());
}

// A plain click takes the focus and follows; a click with Ctrl belongs to the terminal, which opens
// a hyperlink itself and passes the click on, so following it here would open it twice; another
// button is no click on a link.
TRIAL("Link: a click follows it, a click with Ctrl or another button does not") {
    Loop loop;
    ui::Root top;

    const auto opening = std::make_shared<Opening>();
    const auto one = linked("strona", "https://example.org/", opening);

    top.children().add(one);

    (void)top.render({{0, 0}, {10, 1}});

    ui::Mods ctrl;

    ctrl.ctrl = true;

    ui::ClickEvent held{{2, 0}, ui::Mouse::left, ctrl};

    (void)top.dispatch(held);

    ui::ClickEvent right{{2, 0}, ui::Mouse::right};

    (void)top.dispatch(right);

    settled(loop);

    ought(opening->asked.empty());

    ui::ClickEvent plain{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(plain));

    settled(loop);

    same(opening->asked.size(), std::size_t{1});
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

// A disabled link, or one without an address, follows nowhere.
TRIAL("Link: disabled or without an address it follows nowhere") {
    Loop loop;
    ui::Root top;

    const auto opening = std::make_shared<Opening>();
    const auto off = linked("strona", "https://example.org/", opening);
    const auto empty = linked("nic", "", opening);
    const auto row = std::make_shared<ui::Row>();

    row->children().add(off);
    row->children().add(empty);
    top.children().add(row);

    off->enabled(false);
    off->follow();
    empty->follow();

    settled(loop);

    ought(opening->asked.empty());
}

// --- The system launcher -------------------------------------------------------------------------

// Over ssh the browser of this machine is not the user's: the address goes to the clipboard, which
// over ssh reaches the user's terminal.
TRIAL("Launcher: over ssh the address goes to the clipboard") {
    auto line = faking::terminal();
    const auto board = faking::clipboard();

    line->environment["SSH_CONNECTION"] = "10.0.0.2 50000 10.0.0.1 22";
    line->environment["DISPLAY"] = ":0";

    same(static_cast<int>(answered(line, board, "https://example.org/")), static_cast<int>(Launched::copied));
    same(board->kept, std::optional<std::string>{"https://example.org/"});
}

// Only a web page and a mail address are opened; `file:` or a scheme of some program could start
// that program, so such an address goes to the clipboard, and the user takes it where they want.
TRIAL("Launcher: an address that is not a page or a mail address goes to the clipboard") {
    auto line = faking::terminal();
    const auto board = faking::clipboard();

    line->environment["DISPLAY"] = ":0";

    same(static_cast<int>(answered(line, board, "file:///etc/passwd")), static_cast<int>(Launched::copied));
    same(board->kept, std::optional<std::string>{"file:///etc/passwd"});

    same(static_cast<int>(answered(line, board, "ms-settings:privacy")), static_cast<int>(Launched::copied));
}

#if !defined(_WIN32) && !defined(__APPLE__)

// Without X or Wayland there is no window to open the address in.
TRIAL("Launcher: without a graphical session the address goes to the clipboard") {
    auto line = faking::terminal();
    const auto board = faking::clipboard();

    same(static_cast<int>(answered(line, board, "https://example.org/")), static_cast<int>(Launched::copied));
    same(board->kept, std::optional<std::string>{"https://example.org/"});
}

#endif

// An address with a control character is not an address: nothing is opened or copied. A clipboard
// that refuses the copy leaves the address refused too.
TRIAL("Launcher: an address with a control character is refused, and so is a refused copy") {
    auto line = faking::terminal();
    const auto board = faking::clipboard();

    line->environment["SSH_TTY"] = "/dev/pts/3";

    same(static_cast<int>(answered(line, board, "https://example.org/\n")), static_cast<int>(Launched::refused));
    same(static_cast<int>(answered(line, board, "")), static_cast<int>(Launched::refused));
    same(board->copies, 0);

    board->refusing = true;

    same(static_cast<int>(answered(line, board, "https://example.org/")), static_cast<int>(Launched::refused));
}

// --- Addresses in text ---------------------------------------------------------------------------

namespace {

// The addresses `addressed` finds in the text, in order.
std::vector<std::string> found(std::string_view text) {
    std::vector<std::string> made;

    for (const ui::Mark& one : ui::addressed(text, "r")) made.push_back(one.address);

    return made;
}

// The text of a view's row, with the spaces on the right trimmed.
std::string row_of(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x)
        if (const char32_t letter = one.at({x, y}).letter; letter != 0) made += spelled(letter);

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

}

// Every http and https address is found where a word may start, with its line and its bytes counted
// from the line's start, as labels and pages count them; the marks of a sentence after it and a
// parenthesis it did not open stay outside, a parenthesis of its own stays inside.
TRIAL("Addresses: found in text with their lines, without the sentence around them") {
    const std::vector<ui::Mark> marks = ui::addressed("see https://example.org/a.\nand (http://x.example/b) too", "twa.link");

    same(marks.size(), std::size_t{2});
    same(marks[0].line, std::size_t{0});
    same(marks[0].from, std::size_t{4});
    same(marks[0].upto, std::size_t{25});
    same(marks[0].address, std::string{"https://example.org/a"});
    same(marks[0].role, std::string{"twa.link"});
    same(marks[1].line, std::size_t{1});
    same(marks[1].from, std::size_t{5});
    same(marks[1].address, std::string{"http://x.example/b"});

    same(found("https://en.wikipedia.org/wiki/Heat_(1995_film)"), (std::vector<std::string>{"https://en.wikipedia.org/wiki/Heat_(1995_film)"}));
    same(found("„https://pl.wikipedia.org/wiki/Łódź”, i tyle"), (std::vector<std::string>{"https://pl.wikipedia.org/wiki/Łódź"}));
    same(found("<https://example.org/x> \"https://example.org/y\""), (std::vector<std::string>{"https://example.org/x", "https://example.org/y"}));
    ought(found("xhttps://example.org/ https:// ftp://example.org/").empty());
    same(found("HTTPS://Example.org/"), (std::vector<std::string>{"HTTPS://Example.org/"}));
}

// --- Links in a label ----------------------------------------------------------------------------

// A mark lays its role over the text it covers and, with an address, its hyperlink; wrapping joins
// the words with one space where the text had several, and the mark covers the gap too.
TRIAL("Span: a mark with an address lays its role and hyperlink over its text, wrapped too") {
    ui::Root top;

    const auto one = std::make_shared<ui::Span>("go  to example now", ui::SpanShape{.wrapped = true});

    one->marks().add(ui::Mark{0, 4, 14, "talcite.link", "https://example.org/"});

    top.children().add(one);

    const Snapshot made = top.render({{0, 0}, {8, 3}});

    same(row_of(made, 0), std::string{"go to"});
    same(row_of(made, 1), std::string{"example"});
    same(row_of(made, 2), std::string{"now"});

    ought(!made.at({0, 0}).style.under);
    same(made.at({0, 0}).link, std::uint32_t{0});
    ought(made.at({3, 0}).style.under);
    ought(made.at({3, 0}).link != 0);
    ought(made.at({6, 1}).link != 0);
    same(made.at({6, 1}).link, made.at({3, 0}).link);
    same(made.at({0, 2}).link, std::uint32_t{0});
    same(made.links().size(), std::size_t{1});
    same(made.links().front().address, std::string{"https://example.org/"});
}

// A mark without an address only lays its role; a mark over what the ellipsis hides covers nothing.
TRIAL("Span: a mark without an address is only a role, and what is elided carries no link") {
    ui::Root top;

    const auto one = std::make_shared<ui::Span>("abc https://example.org/long", ui::SpanShape{.elide = ui::Elide::right});

    one->marks().all(ui::addressed(one->text(), "talcite.link"));
    one->marks().add(ui::Mark{0, 0, 1, "talcite.link"});

    top.children().add(one);

    const Snapshot made = top.render({{0, 0}, {10, 1}});

    ought(made.at({0, 0}).style.under);
    same(made.at({0, 0}).link, std::uint32_t{0});
    ought(made.at({4, 0}).link != 0);
    same(made.at({9, 0}).letter, char32_t{U'…'});
    same(made.at({9, 0}).link, std::uint32_t{0});
}

// A click on a link of a label follows it; elsewhere, with Ctrl or another button the click is not
// the label's and goes on to whoever holds it.
TRIAL("Span: a click on a link follows it, a click elsewhere is not the label's") {
    Loop loop;
    ui::Root top;

    const auto opening = std::make_shared<Opening>();
    const auto one = std::make_shared<ui::Span>("go to example");

    one->marks().add(ui::Mark{0, 6, 13, "talcite.link", "https://example.org/"});
    one->launcher(Seam<Launcher>{opening});

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 1}});

    ui::ClickEvent beside{{1, 0}, ui::Mouse::left};

    (void)top.dispatch(beside);

    ui::Mods ctrl;

    ctrl.ctrl = true;

    ui::ClickEvent held{{7, 0}, ui::Mouse::left, ctrl};

    (void)top.dispatch(held);

    settled(loop);

    ought(opening->asked.empty());

    ui::ClickEvent on{{7, 0}, ui::Mouse::left};

    ought(top.dispatch(on));
    ought(awaited(loop, [&opening] { return !opening->asked.empty(); }));

    same(opening->asked.front(), std::string{"https://example.org/"});
}

// --- Links in a page -----------------------------------------------------------------------------

// A mark with an address in a page lays its hyperlink over its cells; Enter follows the current
// mark when it leads somewhere, and a click follows the mark under it.
TRIAL("Viewer: a mark with an address is a hyperlink, Enter and a click follow it") {
    Loop loop;
    ui::Root top;

    const auto opening = std::make_shared<Opening>();
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(bytes(std::string{"read https://example.org/a\nor https://example.org/b"}));
    one->marks().all(ui::addressed("read https://example.org/a\nor https://example.org/b", "talcite.viewer.mark"));
    one->launcher(Seam<Launcher>{opening});

    top.children().add(one);

    one->focus();

    const Snapshot made = top.render({{0, 0}, {40, 3}});

    same(made.at({0, 0}).link, std::uint32_t{0});
    ought(made.at({5, 0}).link != 0);
    ought(made.at({3, 1}).link != 0);
    ought(made.at({3, 1}).link != made.at({5, 0}).link);

    ui::KeyEvent enter{ui::Key{ui::Code::enter}};

    // No current mark yet: Enter follows nothing.
    (void)top.dispatch(enter);

    settled(loop);

    ought(opening->asked.empty());

    ought(one->step(1));
    ought(top.dispatch(enter));
    ought(awaited(loop, [&opening] { return opening->asked.size() == 1; }));

    same(opening->asked.front(), std::string{"https://example.org/a"});

    ui::ClickEvent click{{5, 1}, ui::Mouse::left};

    ought(top.dispatch(click));
    ought(awaited(loop, [&opening] { return opening->asked.size() == 2; }));

    same(opening->asked.back(), std::string{"https://example.org/b"});
}
