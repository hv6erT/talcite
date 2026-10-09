#include "proving.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/diff.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"

using namespace talcite;

namespace {

class Sheet : public ui::Root {};

std::string said(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// Tekst strony poza zaznaczeniami i sam tekst zaznaczeń.
std::string kept(const ui::DiffSide& one) {
    std::string made;
    std::size_t at = 0;

    for (const ui::DiffSpan& span : one.marked) {
        made += one.text.substr(at, span.from - at);
        at = span.upto;
    }

    return made + one.text.substr(at);
}

std::string marked(const ui::DiffSide& one) {
    std::string made;

    for (const ui::DiffSpan& span : one.marked) made += "[" + one.text.substr(span.from, span.upto - span.from) + "]";

    return made;
}

// Te same słowa, co w bibliotece — próba liczy najdłuższy wspólny podciąg zwykłą tablicą i sprawdza,
// że Myers go znalazł.
std::vector<std::string> words(std::string_view text) {
    const auto wordy = [](unsigned char sign) {
        return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') || (sign >= 'A' && sign <= 'Z') ||
               sign >= 0x80;
    };

    std::vector<std::string> made;

    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = at + 1;

        if (wordy(static_cast<unsigned char>(text[at])))
            while (end < text.size() && wordy(static_cast<unsigned char>(text[end]))) ++end;

        made.emplace_back(text.substr(at, end - at));
        at = end;
    }

    return made;
}

std::size_t common(const std::vector<std::string>& one, const std::vector<std::string>& two) {
    std::vector<std::vector<std::size_t>> made(one.size() + 1, std::vector<std::size_t>(two.size() + 1, 0));

    for (std::size_t x = 1; x <= one.size(); ++x)
        for (std::size_t y = 1; y <= two.size(); ++y)
            made[x][y] = one[x - 1] == two[y - 1] ? made[x - 1][y - 1] + 1 : std::max(made[x - 1][y], made[x][y - 1]);

    return made[one.size()][two.size()];
}

ui::DiffRow row(std::string_view left, std::string_view right) {
    auto [one, two] = ui::diffed(left, right);

    return ui::DiffRow{std::move(one), std::move(two)};
}

ui::DiffHunk hunk(std::vector<ui::DiffRow> rows, bool tickable = true) {
    return ui::DiffHunk{{}, std::move(rows), tickable, {}};
}

ui::DiffHunk heading(std::string title) { return ui::DiffHunk{std::move(title), {}, false, {}}; }

ui::DiffSource sourced(std::vector<ui::DiffHunk> all) {
    return [all = std::move(all)](std::size_t from, std::size_t most) {
        std::vector<ui::DiffHunk> made;

        for (std::size_t one = from; one < all.size() && made.size() < most; ++one) made.push_back(all[one]);

        return std::optional{made};
    };
}

// Dwie sekcje: dwa kawałki pod pierwszym nagłówkiem, jeden pod drugim.
std::vector<ui::DiffHunk> planned() {
    return {heading("Pewne (2)"),
            hunk({row("a.mkv", "A (2000)/A (2000).mkv"), row("a.srt", "A (2000)/A (2000).srt")}),
            hunk({row("b.mkv", "B (2001)/B (2001).mkv")}),
            heading("Kolizje (1)"),
            hunk({row("c.mkv", "C (2002)/")}, false)};
}

std::shared_ptr<ui::DiffViewer> shown(Sheet& sheet, std::vector<ui::DiffHunk> all) {
    const auto one = std::make_shared<ui::DiffViewer>(ui::DiffShape{false, ui::Elide::middle});

    const std::size_t much = all.size();

    one->source(sourced(std::move(all)), much);

    sheet.children().add(one);

    return one;
}

bool pressed(Sheet& sheet, ui::Key key) {
    ui::KeyEvent what{key};

    return sheet.dispatch(what);
}

}

// --- Różnica napisów -----------------------------------------------------------------------------

TRIAL("Diff: równe napisy nie mają różnic") {
    const auto [one, two] = ui::diffed("Heat (1995).mkv", "Heat (1995).mkv");

    ought(one.marked.empty());
    ought(two.marked.empty());
    same(one.text, std::string{"Heat (1995).mkv"});
}

TRIAL("Diff: przeniesienie do katalogu świeci tylko katalogiem") {
    const auto [one, two] = ui::diffed("a.mkv", "Dir/a.mkv");

    ought(one.marked.empty());
    same(marked(two), std::string{"[Dir/]"});
}

TRIAL("Diff: różnica idzie po słowach, a nie po literach") {
    const auto [one, two] = ui::diffed("The.Matrix.1999.1080p.BluRay.mkv", "The Matrix (1999).mkv");

    same(kept(one), kept(two));
    ought(marked(one).find("1080p.BluRay") != std::string::npos);
    ought(kept(one).find("Matrix") != std::string::npos);
    ought(kept(one).find("1999") != std::string::npos);
}

TRIAL("Diff: ogonki należą do słowa") {
    const auto [one, two] = ui::diffed("zażółć", "zażółć gęślą");

    ought(one.marked.empty());
    same(marked(two), std::string{"[ gęślą]"});
}

TRIAL("Diff: pusta strona to całość po drugiej") {
    const auto [one, two] = ui::diffed("", "abc def");

    ought(one.marked.empty());
    same(marked(two), std::string{"[abc def]"});

    const auto [three, four] = ui::diffed("abc", "");

    same(marked(three), std::string{"[abc]"});
    ought(four.marked.empty());
}

// **Myers znajduje najdłuższy wspólny podciąg**: na losowych napisach z kilku słów niezaznaczone
// części obu stron są równe, a liczba zachowanych słów to dokładnie długość wspólnego podciągu
// policzona tablicą.
TRIAL("Diff: zachowane słowa to najdłuższy wspólny podciąg") {
    std::mt19937 gen(3);

    const std::vector<std::string> pieces{"a", "b", "cd", "ef", ".", " ", "(", ")", "1999", "ż"};

    for (int round = 0; round < 500; ++round) {
        std::string left;
        std::string right;

        const int much_left = static_cast<int>(gen() % 12);
        const int much_right = static_cast<int>(gen() % 12);

        for (int one = 0; one < much_left; ++one) left += pieces[gen() % pieces.size()];
        for (int one = 0; one < much_right; ++one) right += pieces[gen() % pieces.size()];

        const auto [one, two] = ui::diffed(left, right);

        same(kept(one), kept(two));
        same(words(kept(one)).size() <= common(words(left), words(right)), true);
        same(words(left).size() - words(kept(one)).size() + words(right).size() - words(kept(two)).size(),
             words(left).size() + words(right).size() - 2 * common(words(left), words(right)));

        for (const ui::DiffSpan& span : one.marked) ought(span.from < span.upto && span.upto <= left.size());
        for (std::size_t at = 1; at < one.marked.size(); ++at) ought(one.marked[at - 1].upto < one.marked[at].from);
    }
}

// Za dużo różnic: środek między wspólnym początkiem a końcem zaznacza się w całości.
TRIAL("Diff: przy wielu różnicach zaznacza cały środek") {
    std::string left = "X ";
    std::string right = "X ";

    for (int one = 0; one < 400; ++one) {
        left += "a" + std::to_string(one) + " ";
        right += "b" + std::to_string(one) + " ";
    }

    left += "Y";
    right += "Y";

    const auto [one, two] = ui::diffed(left, right);

    same(one.marked.size(), std::size_t{1});
    same(one.marked.front().from, std::size_t{2});
    same(one.marked.front().upto, left.size() - 2);
    same(two.marked.size(), std::size_t{1});
    same(kept(one), std::string{"X  Y"});
}

// --- Widok ---------------------------------------------------------------------------------------

TRIAL("DiffViewer: pusty nic nie niesie") {
    const ui::DiffViewer one;

    same(one.hunks(), std::size_t{0});
    ought(!one.which().has_value());
    ought(!one.unified().has_value());
    ought(one.scrollbar());
}

// Pasek stoi tylko wtedy, gdy jest czym przewijać: plan, który mieści się cały, go nie ma; plan
// o wiersz dłuższy — ma. Kolumna na pasek zostaje, żeby strony nie skakały.
TRIAL("DiffViewer: pasek tylko wtedy, gdy plan się nie mieści") {
    const auto barred = [](std::vector<ui::DiffHunk> all, int high) {
        Sheet sheet;

        const auto one = std::make_shared<ui::DiffViewer>();
        const std::size_t much = all.size();

        one->source(sourced(std::move(all)), much);
        sheet.children().add(one);

        const View made = sheet.render({{0, 0}, {70, high}});

        for (int y = 0; y < high; ++y)
            if (made.at({69, y}).letter != U' ') return true;

        return false;
    };

    // Plan: dwa nagłówki sekcji i trzy kawałki (pierwszy zwinięty) — pięć wierszy.
    ought(!barred(planned(), 5));
    ought(!barred(planned(), 12));
    ought(barred(planned(), 4));

    std::vector<ui::DiffHunk> long_plan;

    for (int one = 0; one < 30; ++one) long_plan.push_back(hunk({row("x" + std::to_string(one), "y")}));

    ought(barred(long_plan, 10));

    // Na końcu długiego planu reszta się mieści, ale początek jest nad oknem — pasek zostaje.
    {
        Sheet sheet;

        const auto one = std::make_shared<ui::DiffViewer>();
        const std::size_t much = long_plan.size();

        one->source(sourced(long_plan), much);
        sheet.children().add(one);

        (void)sheet.render({{0, 0}, {70, 10}});

        one->focus();

        ought(pressed(sheet, ui::Code::end));

        const View made = sheet.render({{0, 0}, {70, 10}});

        bool bar = false;

        for (int y = 0; y < 10; ++y) bar = bar || made.at({69, y}).letter != U' ';

        ought(bar);
    }

    // Źródło, które każe czekać na dalsze kawałki, też daje pasek.
    {
        Sheet sheet;

        const auto one = std::make_shared<ui::DiffViewer>();

        one->source(
            [](std::size_t from, std::size_t) -> std::optional<std::vector<ui::DiffHunk>> {
                if (from >= 2) return std::nullopt;

                return std::vector<ui::DiffHunk>{hunk({row("a", "b")}), hunk({row("c", "d")})};
            },
            5);
        sheet.children().add(one);

        const View made = sheet.render({{0, 0}, {70, 10}});

        bool bar = false;

        for (int y = 0; y < 10; ++y) bar = bar || made.at({69, y}).letter != U' ';

        ought(bar);
    }
}

// Nagłówek z tytułami stron, tytuł sekcji we własnym wierszu, pola wyboru przy kawałkach, które je
// mają, i „+N" przy zwiniętym kawałku z kilkoma wierszami. Kursor stoi na pierwszym kawałku
// z wierszami, a nie na nagłówku sekcji.
TRIAL("DiffViewer: dwie strony, sekcje, pola wyboru i zwinięcie") {
    Sheet sheet;

    const auto one = shown(sheet, planned());

    one->titles("Było", "Będzie");

    const View made = sheet.render({{0, 0}, {70, 8}});

    same(one->which(), std::optional<std::size_t>{1});

    ought(said(made, 0).find("Było") != std::string::npos);
    ought(said(made, 0).find("Będzie") != std::string::npos);
    ought(said(made, 1).find("Pewne (2)") != std::string::npos);
    ought(said(made, 2).rfind("[ ] a.mkv", 0) == 0);
    ought(said(made, 2).find("+1") != std::string::npos);
    ought(said(made, 2).find("A (2000)/A (2000).mkv") != std::string::npos);
    ought(said(made, 3).rfind("[ ] b.mkv", 0) == 0);
    ought(said(made, 4).find("Kolizje (1)") != std::string::npos);
    ought(said(made, 5).rfind("    c.mkv", 0) == 0);
    same(said(made, 6), std::string{});

    one->opened(1, true);

    const View open = sheet.render({{0, 0}, {70, 8}});

    ought(said(open, 2).find("+1") == std::string::npos);
    ought(said(open, 3).find("a.srt") != std::string::npos);
    ought(said(open, 3).find("A (2000).srt") != std::string::npos);
    ought(one->opened(1));
    ought(!one->opened(2));
}

// Wąsko: widok sam przechodzi na jedną kolumnę — strona usunięta z minusem, dodana z plusem, wiersz
// równy raz, bez znaku. Na życzenie da się wymusić każdy z widoków.
TRIAL("DiffViewer: wąski widok przechodzi na jedną kolumnę") {
    Sheet sheet;

    const auto one = shown(sheet, {hunk({row("a.mkv", "A/a.mkv"), row("same.txt", "same.txt")}, false)});

    one->opened(0, true);

    const View narrow = sheet.render({{0, 0}, {40, 5}});

    same(said(narrow, 0), std::string{"- a.mkv"});
    same(said(narrow, 1), std::string{"+ A/a.mkv"});
    same(said(narrow, 2), std::string{"  same.txt"});

    one->unified(false);

    const View split = sheet.render({{0, 0}, {40, 5}});

    ought(said(split, 0).rfind("a.mkv", 0) == 0);
    ought(said(split, 0).find("A/a.mkv") != std::string::npos);

    one->unified(true);

    same(one->unified(), std::optional<bool>{true});

    const View wide = sheet.render({{0, 0}, {120, 5}});

    same(said(wide, 0), std::string{"- a.mkv"});

    one->unified(std::nullopt);

    ought(said(sheet.render({{0, 0}, {120, 5}}), 0).find("A/a.mkv") != std::string::npos);
}

// Spacja przestawia pole kawałka pod kursorem i zawiadamia; na kawałku bez pola nic nie robi.
// `+` i `-` przestawiają całą sekcję kursora — i tylko ją.
TRIAL("DiffViewer: spacja, plus i minus przestawiają pola") {
    Sheet sheet;

    std::vector<ui::DiffHunk> all = planned();

    all.push_back(hunk({row("d.mkv", "D/d.mkv")}));

    const auto one = shown(sheet, all);

    (void)sheet.render({{0, 0}, {70, 10}});

    one->focus();

    int heard = 0;

    (void)sheet.on<ui::PickEvent>([&heard](ui::PickEvent&) {
        ++heard;

        return false;
    });

    ought(pressed(sheet, ui::Key{U' '}));
    ought(one->chosen().contains(1));
    same(heard, 1);

    ought(pressed(sheet, ui::Key{U'+'}));
    ought(one->chosen().contains(2));
    ought(!one->chosen().contains(5));
    same(heard, 2);

    ought(pressed(sheet, ui::Key{U'+'}));
    same(heard, 2);

    one->which(4);

    ought(pressed(sheet, ui::Key{U' '}));
    ought(!one->chosen().contains(4));
    same(heard, 2);

    ought(pressed(sheet, ui::Key{U'+'}));
    ought(one->chosen().contains(5));
    ought(one->chosen().contains(1));
    same(one->chosen().many(), std::size_t{3});

    one->which(1);

    ought(pressed(sheet, ui::Key{U'-'}));
    same(one->chosen().many(), std::size_t{1});
    ought(one->chosen().contains(5));

    ought(said(sheet.render({{0, 0}, {70, 10}}), 5).rfind("[x] d.mkv", 0) == 0);
}

// Strzałki chodzą po kawałkach z wierszami, nagłówki przeskakują; Ctrl ze strzałką skacze po
// sekcjach; strzałki w bok zwijają i rozwijają. Tab i Enter idą wyżej.
TRIAL("DiffViewer: klawisze chodzą po kawałkach i sekcjach") {
    Sheet sheet;

    std::vector<ui::DiffHunk> all = planned();

    all.push_back(hunk({row("d.mkv", "D/d.mkv")}));

    const auto one = shown(sheet, all);

    (void)sheet.render({{0, 0}, {70, 10}});

    one->focus();

    int moved = 0;

    (void)sheet.on<ui::ScrollEvent>([&moved](ui::ScrollEvent&) {
        ++moved;

        return false;
    });

    ought(pressed(sheet, ui::Code::down));
    same(one->which(), std::optional<std::size_t>{2});
    same(moved, 1);

    ought(pressed(sheet, ui::Code::down));
    same(one->which(), std::optional<std::size_t>{4});

    ought(pressed(sheet, ui::Code::up));
    same(one->which(), std::optional<std::size_t>{2});

    ought(pressed(sheet, ui::Key{ui::Code::down, ui::Mods{.ctrl = true}}));
    same(one->which(), std::optional<std::size_t>{4});

    ought(pressed(sheet, ui::Key{ui::Code::down, ui::Mods{.ctrl = true}}));
    same(one->which(), std::optional<std::size_t>{4});

    one->which(5);

    ought(pressed(sheet, ui::Key{ui::Code::up, ui::Mods{.ctrl = true}}));
    same(one->which(), std::optional<std::size_t>{4});

    ought(pressed(sheet, ui::Key{ui::Code::up, ui::Mods{.ctrl = true}}));
    same(one->which(), std::optional<std::size_t>{1});

    ought(pressed(sheet, ui::Code::end));
    same(one->which(), std::optional<std::size_t>{5});

    ought(pressed(sheet, ui::Code::home));
    same(one->which(), std::optional<std::size_t>{1});

    ought(pressed(sheet, ui::Code::right));
    ought(one->opened(1));

    ought(pressed(sheet, ui::Code::left));
    ought(!one->opened(1));

    ought(!pressed(sheet, ui::Code::tab));
    ought(!pressed(sheet, ui::Code::enter));
    ought(!pressed(sheet, ui::Key{U'x', ui::Mods{.alt = true}}));
}

// Długa lista: kursor w dół ciągnie widok, a koniec staje na dole ekranu, nie na górze z pustką
// pod nim. Kółko przesuwa sam widok.
TRIAL("DiffViewer: widok idzie za kursorem i nie wychodzi za koniec") {
    Sheet sheet;

    std::vector<ui::DiffHunk> all;

    for (int one = 0; one < 30; ++one) all.push_back(hunk({row("f" + std::to_string(one), "F" + std::to_string(one))}));

    const auto one = shown(sheet, all);

    one->unified(false);

    (void)sheet.render({{0, 0}, {40, 5}});

    one->focus();

    for (int step = 0; step < 7; ++step) ought(pressed(sheet, ui::Code::down));

    same(one->which(), std::optional<std::size_t>{7});

    const View seen = sheet.render({{0, 0}, {40, 5}});

    ought(said(seen, 4).rfind("[ ] f7", 0) == 0);
    ought(said(seen, 0).rfind("[ ] f3", 0) == 0);

    ought(pressed(sheet, ui::Code::end));

    const View last = sheet.render({{0, 0}, {40, 5}});

    ought(said(last, 4).rfind("[ ] f29", 0) == 0);
    ought(said(last, 0).rfind("[ ] f25", 0) == 0);

    ought(pressed(sheet, ui::Code::home));

    ui::WheelEvent wheel{{0, 0}, -2};

    ought(sheet.dispatch(wheel));
    same(one->which(), std::optional<std::size_t>{0});
    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f6", 0) == 0);

    one->which(29);

    ui::WheelEvent more{{0, 0}, -2};

    ought(!sheet.dispatch(more));
}

// Numery wierszy stoją przy stronach, które je niosą, wyrównane do największego widocznego.
// Strona pusta — wiersz dodany albo usunięty — zostaje pusta.
TRIAL("DiffViewer: numery wierszy i strony puste") {
    Sheet sheet;

    ui::DiffSide nine{"dziewięć", {}, 9};
    ui::DiffSide ten{"dziesięć", {}, 10};
    ui::DiffSide added{"nowy", {ui::DiffSpan{0, 4}}, 11};

    const auto one = shown(sheet, {hunk({ui::DiffRow{nine, nine}, ui::DiffRow{ten, std::nullopt},
                                         ui::DiffRow{std::nullopt, added}},
                                        false)});

    one->opened(0, true);
    one->unified(false);

    const View made = sheet.render({{0, 0}, {60, 4}});

    ought(said(made, 0).rfind(" 9 dziewięć", 0) == 0);
    ought(said(made, 1).rfind("10 dziesięć", 0) == 0);
    ought(said(made, 2).find("11 nowy") != std::string::npos);
    ought(said(made, 2).rfind("  ", 0) == 0);

    one->unified(true);

    const View flat = sheet.render({{0, 0}, {60, 4}});

    same(said(flat, 0), std::string{" 9  9   dziewięć"});
    same(said(flat, 1), std::string{"10    - dziesięć"});
    same(said(flat, 2), std::string{"   11 + nowy"});
}

// Skrót w środku zostawia zaznaczenia przy swoich literach: ogon po skróceniu dalej świeci jako
// dodany, a początek, wspólny z drugą stroną, nie.
TRIAL("DiffViewer: skrót w środku zostawia zaznaczenia przy literach") {
    Sheet sheet;

    const std::string long_one = "wspolny/poczatek/bardzo/dlugiej/sciezki/az/do/pliku.mkv";

    const auto one = shown(sheet, {hunk({row("pliku.mkv", long_one)}, false)});

    one->unified(true);

    const View made = sheet.render({{0, 0}, {24, 2}});

    const std::string line = said(made, 1);

    ought(line.find("…") != std::string::npos || line.find("~") != std::string::npos);
    ought(line.find("pliku.mkv") != std::string::npos);

    // Początek dodanej ścieżki świeci, bo pierwszej strony tam nie ma; ogon nie, bo jest wspólny.
    const Cell head = made.at({2, 1});
    const Cell tail = made.at({23, 1});

    ought(head.style.bold);
    ought(!tail.style.bold);
}

// Źródło, które każe czekać, zostawia pusty widok; po `refresh()` widać to, co doszło.
TRIAL("DiffViewer: źródło, które czeka, i odświeżenie") {
    Sheet sheet;

    bool ready = false;

    const auto one = std::make_shared<ui::DiffViewer>(ui::DiffShape{false, ui::Elide::middle});

    one->source(
        [&ready](std::size_t, std::size_t) -> std::optional<std::vector<ui::DiffHunk>> {
            if (!ready) return {};

            return std::vector<ui::DiffHunk>{hunk({row("a", "b")})};
        },
        1);

    sheet.children().add(one);

    ought(!one->which().has_value());
    same(said(sheet.render({{0, 0}, {40, 3}}), 0), std::string{});

    ready = true;

    one->refresh();

    ought(said(sheet.render({{0, 0}, {40, 3}}), 0).rfind("[ ] - a", 0) == 0);
    same(one->which(), std::optional<std::size_t>{0});
}

// Klik stawia kursor na kawałku pod nim, a klik w pole przestawia je. Klik w nagłówek sekcji nic.
TRIAL("DiffViewer: klik stawia kursor i przestawia pole") {
    Sheet sheet;

    const auto one = shown(sheet, planned());

    (void)sheet.render({{0, 0}, {70, 8}});

    ui::ClickEvent row_click{{10, 2}, ui::Mouse::left};

    ought(sheet.dispatch(row_click));
    same(one->which(), std::optional<std::size_t>{2});
    ought(one->chosen().empty());

    ui::ClickEvent box{{1, 1}, ui::Mouse::left};

    ought(sheet.dispatch(box));
    same(one->which(), std::optional<std::size_t>{1});
    ought(one->chosen().contains(1));

    ui::ClickEvent title{{1, 0}, ui::Mouse::left};

    ought(sheet.dispatch(title));
    same(one->which(), std::optional<std::size_t>{1});
}

// Nowe źródło czyści zaznaczenia i zwinięcia; mniej kawałków przycina zaznaczenia przy malowaniu.
TRIAL("DiffViewer: nowe źródło zaczyna od czystego") {
    Sheet sheet;

    const auto one = shown(sheet, planned());

    one->chosen().add(1);
    one->opened(1, true);

    one->source(sourced(planned()), 5);

    ought(one->chosen().empty());
    ought(!one->opened(1));

    one->chosen().add(4);

    one->source(sourced({hunk({row("a", "b")})}), 1);

    one->chosen().add(0);

    (void)sheet.render({{0, 0}, {40, 3}});

    same(one->chosen().many(), std::size_t{1});
    same(one->hunks(), std::size_t{1});
}

// Litera, której drugi bajt to 0x80 (`Ā`), dalej należy do słowa — słowo różne w całości świeci
// w całości, a nie od połowy litery.
TRIAL("Diff: litera z bajtem 0x80 nie rozcina słowa") {
    const auto [one, two] = ui::diffed("\xc4\x80" "a", "\xc4\x80" "b");

    same(marked(one), std::string{"[\xc4\x80" "a]"});
    same(marked(two), std::string{"[\xc4\x80" "b]"});
}

// Środek dłuższy niż granica zaznacza się w całości, choćby różnic było mało — tu różnią się
// tylko pierwsze i ostatnie słowo, więc wspólnego początku ani końca nie ma.
TRIAL("Diff: bardzo długi środek zaznacza się w całości") {
    std::string middle;

    for (int one = 0; one < 3000; ++one) middle += " w" + std::to_string(one);

    const auto [one, two] = ui::diffed("A" + middle + " B", "C" + middle + " D");

    same(one.marked.size(), std::size_t{1});
    same(one.marked.front().from, std::size_t{0});
    same(one.marked.front().upto, one.text.size());

    const auto [three, four] = ui::diffed("A w1 w2 B", "C w1 w2 D");

    same(marked(three), std::string{"[A][B]"});
    same(marked(four), std::string{"[C][D]"});
}

// Kawałek z tytułem i wierszami (tak wygląda kawałek porównania plików: `@@ ... @@` i wiersze pod
// nim) pokazuje tytuł we własnym wierszu; kawałek zupełnie pusty to pusty wiersz, przez który
// kursor przechodzi; tabulator rozwija się do ósemki; zaznaczenie w środku napisu świeci tylko
// tam, gdzie jest.
TRIAL("DiffViewer: tytuł nad wierszami, pusty kawałek, tabulator i zaznaczenie w środku") {
    Sheet sheet;

    ui::DiffHunk titled{"@@ -1,2 +1,2 @@", {row("x a", "x b a"), row("t\tu", "t\tu")}, false, {}};

    const auto one = shown(sheet, {titled, ui::DiffHunk{}, hunk({row("k", "k")}, false)});

    one->opened(0, true);
    one->unified(true);

    const View made = sheet.render({{0, 0}, {40, 8}});

    same(said(made, 0), std::string{"@@ -1,2 +1,2 @@"});
    same(said(made, 1), std::string{"- x a"});
    same(said(made, 2), std::string{"+ x b a"});
    same(said(made, 3), std::string{"  t       u"});
    same(said(made, 4), std::string{});
    same(said(made, 5), std::string{"  k"});

    ought(!made.at({2, 2}).style.bold);
    ought(made.at({4, 2}).style.bold);
    ought(!made.at({6, 2}).style.bold);

    one->focus();

    same(one->which(), std::optional<std::size_t>{0});

    ought(pressed(sheet, ui::Code::down));
    same(one->which(), std::optional<std::size_t>{2});
}

// Kawałek przewinięty kółkiem do połowy: skok kursora na niego stawia jego początek na górze.
// Klik w miejscu pola przy kawałku bez pola niczego nie zaznacza.
TRIAL("DiffViewer: skok na kawałek przewinięty do połowy i klik obok pola") {
    Sheet sheet;

    std::vector<ui::DiffRow> rows;

    for (int one = 0; one < 6; ++one) rows.push_back(row("r" + std::to_string(one), "r" + std::to_string(one)));

    const auto one = shown(sheet, {hunk(rows, false), hunk({row("n", "m")}, false), hunk({row("o", "o")}, false),
                                   hunk({row("p", "p")}, false)});

    one->opened(0, true);
    one->unified(false);

    (void)sheet.render({{0, 0}, {40, 4}});

    ui::WheelEvent wheel{{0, 0}, -1};

    ought(sheet.dispatch(wheel));
    ought(said(sheet.render({{0, 0}, {40, 4}}), 0).rfind("r3", 0) == 0);

    one->which(1);
    one->which(0);

    ought(said(sheet.render({{0, 0}, {40, 4}}), 0).rfind("r0", 0) == 0);

    ui::ClickEvent box{{1, 0}, ui::Mouse::left};

    ought(sheet.dispatch(box));
    ought(one->chosen().empty());
}

// Mysz na pasku różnic (U6 w twa): strzałka o wiersz, tor o stronę, suwak na kawałek — widok się
// rusza, kursor nie. Klik na pasku nie stawia kursora na wierszu pod nim.
TRIAL("DiffViewer: mysz na pasku przewija widok, kursora nie rusza") {
    Sheet sheet;

    std::vector<ui::DiffHunk> all;

    for (int one = 0; one < 30; ++one) all.push_back(hunk({row("f" + std::to_string(one), "F" + std::to_string(one))}));

    const auto one = std::make_shared<ui::DiffViewer>(ui::DiffShape{true, ui::Elide::middle});

    one->source(sourced(std::move(all)), 30);
    one->unified(false);

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {40, 5}});

    one->focus();

    const auto clicked = [&sheet](ui::Spot at) {
        ui::MouseEvent down{at, ui::Press::down, ui::Mouse::left};
        ui::MouseEvent up{at, ui::Press::up, ui::Mouse::left};

        (void)sheet.dispatch(down);
        (void)sheet.dispatch(up);
    };

    clicked({39, 4});

    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f1", 0) == 0);

    clicked({39, 3});

    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f6", 0) == 0);

    clicked({39, 0});

    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f5", 0) == 0);

    ui::MouseEvent grab{{39, 1}, ui::Press::down, ui::Mouse::left};
    ui::MouseEvent pull{{39, 9}, ui::Press::move, ui::Mouse::left};
    ui::MouseEvent drop{{39, 9}, ui::Press::up, ui::Mouse::left};

    (void)sheet.dispatch(grab);
    (void)sheet.dispatch(pull);
    (void)sheet.dispatch(drop);

    ought(said(sheet.render({{0, 0}, {40, 5}}), 0).rfind("[ ] f25", 0) == 0);

    same(one->which(), std::optional<std::size_t>{0});
}
