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
#include "talcite/launcher.hpp"
#include "talcite/link.hpp"
#include "talcite/loop.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"

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
    Opened answer = Opened::opened;

    // The launcher's name.
    [[nodiscard]] std::string_view name() const override { return "fake launcher"; }

    // Keeps the address and answers at once.
    [[nodiscard]] Promise<Opened> open(std::string address) override {
        asked.push_back(std::move(address));

        return ready(answer);
    }
};

// A link to `address` with `text`, behind the fake launcher `with`.
std::shared_ptr<ui::Link> linked(std::string text, std::string address, const std::shared_ptr<Opening>& with) {
    return std::make_shared<ui::Link>(std::move(text), std::move(address), Seam<Launcher>{with});
}

// What the launcher behind the console and the clipboard answers about the address.
Opened answered(const std::shared_ptr<faking::Terminal>& line, const std::shared_ptr<faking::Clipboard>& board, std::string address) {
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

    const View made = top.render({{0, 0}, {10, 1}});

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

    const View made = top.render({{0, 0}, {6, 1}});

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

    const View focused = top.render({{0, 0}, {10, 1}});

    ought(focused.at({0, 0}).style.reverse);
    ought(focused.at({0, 0}).style.under);

    one->enabled(false);

    const View off = top.render({{0, 0}, {10, 1}});

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

    const View after = top.render({{0, 0}, {10, 1}});

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

    opening->answer = Opened::copied;

    const auto one = linked("strona", "https://example.org/", opening);

    top.children().add(one);

    one->focus();

    std::optional<Opened> told;

    const ui::Hook heard = one->on<ui::FollowedEvent>([&told](ui::FollowedEvent& what) {
        told = what.how();

        same(what.address(), std::string{"https://example.org/"});

        return true;
    });

    ui::KeyEvent enter{ui::Key{ui::Code::enter}};

    ought(top.dispatch(enter));
    ought(awaited(loop, [&told] { return told.has_value(); }));

    same(opening->asked, (std::vector<std::string>{"https://example.org/"}));
    same(static_cast<int>(told.value_or(Opened::refused)), static_cast<int>(Opened::copied));
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

    same(static_cast<int>(answered(line, board, "https://example.org/")), static_cast<int>(Opened::copied));
    same(board->kept, std::optional<std::string>{"https://example.org/"});
}

// Only a web page and a mail address are opened; `file:` or a scheme of some program could start
// that program, so such an address goes to the clipboard, and the user takes it where they want.
TRIAL("Launcher: an address that is not a page or a mail address goes to the clipboard") {
    auto line = faking::terminal();
    const auto board = faking::clipboard();

    line->environment["DISPLAY"] = ":0";

    same(static_cast<int>(answered(line, board, "file:///etc/passwd")), static_cast<int>(Opened::copied));
    same(board->kept, std::optional<std::string>{"file:///etc/passwd"});

    same(static_cast<int>(answered(line, board, "ms-settings:privacy")), static_cast<int>(Opened::copied));
}

#if !defined(_WIN32) && !defined(__APPLE__)

// Without X or Wayland there is no window to open the address in.
TRIAL("Launcher: without a graphical session the address goes to the clipboard") {
    auto line = faking::terminal();
    const auto board = faking::clipboard();

    same(static_cast<int>(answered(line, board, "https://example.org/")), static_cast<int>(Opened::copied));
    same(board->kept, std::optional<std::string>{"https://example.org/"});
}

#endif

// An address with a control character is not an address: nothing is opened or copied. A clipboard
// that refuses the copy leaves the address refused too.
TRIAL("Launcher: an address with a control character is refused, and so is a refused copy") {
    auto line = faking::terminal();
    const auto board = faking::clipboard();

    line->environment["SSH_TTY"] = "/dev/pts/3";

    same(static_cast<int>(answered(line, board, "https://example.org/\n")), static_cast<int>(Opened::refused));
    same(static_cast<int>(answered(line, board, "")), static_cast<int>(Opened::refused));
    same(board->copies, 0);

    board->refusing = true;

    same(static_cast<int>(answered(line, board, "https://example.org/")), static_cast<int>(Opened::refused));
}
