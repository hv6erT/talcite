#include "tal/testing.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "talcite/bytes.hpp"

#include "quilt.hpp"

using namespace talcite;

namespace {



// The quilt's whole current text as a string.
std::string whole(Quilt& one) { return text(one.bytes(0, one.whole())); }

}

TRIAL("Quilt: an empty one carries nothing") {
    Quilt one{bytes("")};

    SAME(one.whole(), std::size_t{0});
    SAME(whole(one), std::string{});
}

TRIAL("Quilt: untouched, it gives back the original") {
    Quilt one{bytes("abcdef")};

    SAME(one.whole(), std::size_t{6});
    SAME(whole(one), std::string{"abcdef"});
    SAME(text(one.bytes(2, 3)), std::string{"cde"});
    SAME(text(one.bytes(4, 100)), std::string{"ef"});
}

TRIAL("Quilt: inserting at the start, in the middle and at the end") {
    Quilt one{bytes("abc")};

    one.insert(0, "X");
    SAME(whole(one), std::string{"Xabc"});

    one.insert(2, "Y");
    SAME(whole(one), std::string{"XaYbc"});

    one.insert(one.whole(), "Z");
    SAME(whole(one), std::string{"XaYbcZ"});

}

TRIAL("Quilt: erasing at the start, in the middle and at the end") {
    Quilt one{bytes("abcdef")};

    one.erase(0, 1);
    SAME(whole(one), std::string{"bcdef"});

    one.erase(2, 2);
    SAME(whole(one), std::string{"bcf"});

    one.erase(2, 100);
    SAME(whole(one), std::string{"bc"});
}

TRIAL("Quilt: inserting and erasing returns to the original") {
    Quilt one{bytes("abcdef")};

    one.insert(3, "XYZ");
    SAME(whole(one), std::string{"abcXYZdef"});

    one.erase(3, 3);
    SAME(whole(one), std::string{"abcdef"});
}

// Typing in a row extends the last piece instead of making a new one. Without that, five thousand
// characters would make five thousand pieces, and this trial is the one that would catch it.
TRIAL("Quilt: five thousand characters one at a time") {
    Quilt one{bytes("")};

    std::string want;

    for (int which = 0; which < 5000; ++which) {
        one.insert(one.whole(), "x");
        want.push_back('x');
    }

    SAME(one.whole(), std::size_t{5000});
    SAME(whole(one), want);
}

// Comparison with `std::string` on a random series of changes; it is the only way to catch a bug in
// splitting pieces that no single case shows.
TRIAL("Quilt: random changes agree with the model") {
    std::mt19937 gen(7);

    for (int round = 0; round < 200; ++round) {
        std::string want;

        const int much = static_cast<int>(gen() % 60);

        for (int one = 0; one < much; ++one)
            want.push_back(static_cast<char>('a' + gen() % 26));

        Quilt one{bytes(want)};

        for (int step = 0; step < 30; ++step) {
            if (gen() % 2 || want.empty()) {
                const std::size_t at = want.empty() ? 0 : gen() % (want.size() + 1);

                std::string what;

                const int size = 1 + static_cast<int>(gen() % 5);

                for (int which = 0; which < size; ++which)
                    what.push_back(static_cast<char>('A' + gen() % 26));

                want.insert(at, what);
                one.insert(at, what);
            } else {
                const std::size_t at = gen() % want.size();
                const std::size_t size = 1 + gen() % (want.size() - at);

                want.erase(at, size);
                one.erase(at, size);
            }

            SAME(one.whole(), want.size());
            SAME(whole(one), want);
        }
    }
}

// Many cuts at once agree with the same done on a string: cuts at the start, at the end, right next
// to each other, a pure insertion (nothing goes away), a pure erasure (nothing comes in), the same
// text in many places. The quilt is first cut up by ordinary changes, so that the cuts hit the
// middles of pieces of both kinds.
TRIAL("Quilt: many cuts at once agree with the model") {
    std::mt19937 gen{7};

    for (int round = 0; round < 200; ++round) {
        std::string want;

        for (int at = 0; at < 40; ++at) want.push_back(static_cast<char>('a' + gen() % 26));

        Quilt one{bytes(want)};

        for (int step = 0; step < 5; ++step) {
            const std::size_t at = gen() % (want.size() + 1);

            want.insert(at, "XY");
            one.insert(at, "XY");
        }

        std::vector<std::string> texts;
        std::vector<Quilt::Cut> cuts;
        std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> plan;

        for (std::size_t at = gen() % 3; at <= want.size();) {
            const std::size_t much = std::min<std::size_t>(gen() % 4, want.size() - at);

            plan.emplace_back(at, much, gen() % 3);

            at += much + gen() % 5;

            if (much == 0) ++at;
        }

        texts.reserve(plan.size());

        for (const auto& [at, much, kind] : plan) {
            texts.push_back(kind == 0 ? std::string{} : kind == 1 ? std::string{"++"} : std::string(1 + gen() % 3, '#'));
            cuts.push_back(Quilt::Cut{at, much, texts.back()});
        }

        std::string made;
        std::size_t from = 0;

        for (const Quilt::Cut& cut : cuts) {
            made += want.substr(from, cut.at - from);
            made += std::string{cut.text};
            from = cut.at + cut.much;
        }

        made += want.substr(from);

        one.spliced(cuts);

        SAME(one.whole(), made.size());
        SAME(whole(one), made);

        for (std::size_t at = 0; at < made.size(); at += 3) SAME(text(one.bytes(at, 5)), made.substr(at, 5));
    }
}

// The same text in many cuts in a row lands in the additions once: typing on after the last of them
// extends only its piece, and the other places stay as they were.
TRIAL("Quilt: the shared text of cuts does not leak to neighbors when typing on") {
    Quilt one{bytes("a-b-c")};

    const std::string with = "XX";

    one.spliced({Quilt::Cut{1, 1, with}, Quilt::Cut{3, 1, with}});

    SAME(whole(one), std::string{"aXXbXXc"});

    one.insert(6, "!");
    one.insert(3, "?");

    SAME(whole(one), std::string{"aXX?bXX!c"});
}

// Scraps agree with the model. Random insertions, erasures and moves by scraps: a taken scrap has
// the text of the range, the same scrap put elsewhere (also twice) gives what inserting its text
// does, and joining at the front and at the back gives the joined text. Line ends and the last byte
// are counted from the sources. All on a quilt cut up by ordinary changes, so that scraps catch
// pieces of both kinds.
TRIAL("Quilt: scraps agree with the model") {
    std::mt19937 gen(11);

    const auto drawn = [&gen](int most) {
        std::string made;

        const int much = 1 + static_cast<int>(gen() % static_cast<unsigned>(most));

        for (int one = 0; one < much; ++one) made.push_back(gen() % 5 == 0 ? '\n' : static_cast<char>('a' + gen() % 26));

        return made;
    };

    for (int round = 0; round < 200; ++round) {
        std::string want = drawn(60);

        Quilt one{bytes(want)};

        Quilt::Scrap kept;
        std::string told;

        for (int step = 0; step < 30; ++step) {
            const unsigned what = gen() % 5;

            if (what == 0 || want.empty()) {
                const std::size_t at = want.empty() ? 0 : gen() % (want.size() + 1);
                const std::string more = drawn(5);

                want.insert(at, more);
                one.put(at, one.stored(more));
            } else if (what == 1) {
                const std::size_t at = gen() % want.size();
                const std::size_t size = 1 + gen() % (want.size() - at);

                const Quilt::Scrap cut = one.taken(at, size);

                SAME(cut.much, size);
                SAME(one.text(cut), want.substr(at, size));
                SAME(one.lines(cut), static_cast<std::size_t>(std::count(want.begin() + static_cast<std::ptrdiff_t>(at),
                                                                        want.begin() + static_cast<std::ptrdiff_t>(at + size), '\n')));
                SAME(one.last(cut), want[at + size - 1]);

                const bool front = gen() % 2 == 0;

                Quilt::joined(kept, cut, front);
                told = front ? want.substr(at, size) + told : told + want.substr(at, size);

                SAME(one.text(kept), told);
                SAME(kept.much, told.size());

                want.erase(at, size);
                one.erase(at, size);
            } else if (what == 2 && kept.much > 0) {
                const std::size_t at = gen() % (want.size() + 1);

                want.insert(at, told);
                one.put(at, kept);
            } else {
                const std::size_t at = gen() % want.size();
                const std::size_t size = 1 + gen() % (want.size() - at);
                const std::size_t target = gen() % (want.size() + 1);

                const std::string moved = want.substr(at, size);

                want.insert(target, moved);
                one.put(target, one.taken(at, size));
            }

            SAME(one.whole(), want.size());
            SAME(whole(one), want);
        }
    }

    Quilt none{bytes("abc")};

    SAME(none.last(Quilt::Scrap{}), '\0');
    SAME(none.taken(3, 5).much, std::size_t{0});
    SAME(none.taken(1, 0).much, std::size_t{0});
    OUGHT(none.taken(1, 0).pieces.empty());
    OUGHT(none.stored("").pieces.empty());
    SAME(none.taken(1, 100).much, std::size_t{2});
}

// Erasing and undoing it copy no text and stitch the document back together. The additions do not
// grow when a scrap is taken or put, even a hundred times; and after the taken scrap is put back in
// the same place, the document is a single piece again, so undo does not leave a cut-up quilt
// behind. The same for added text: taken and put back, it merges with its neighbors into one piece
// of additions.
TRIAL("Quilt: a scrap returns without a copy and stitches the pieces") {
    std::string doc;

    for (int line = 0; line < 20000; ++line) doc += "wiersz " + std::to_string(line) + "\n";

    Quilt one{bytes(doc)};

    for (int round = 0; round < 100; ++round) {
        const Quilt::Scrap cut = one.taken(1000, 100000);

        one.erase(1000, 100000);
        one.put(1000, cut);
    }

    SAME(one.appended(), std::size_t{0});
    SAME(whole(one), doc);
    SAME(one.taken(0, one.whole()).pieces.size(), std::size_t{1});

    one.insert(500, "abcdef");

    const Quilt::Scrap mid = one.taken(502, 2);

    one.erase(502, 2);

    SAME(one.taken(0, one.whole()).pieces.size(), std::size_t{4});

    one.put(502, mid);

    SAME(one.appended(), std::size_t{6});
    SAME(one.taken(0, one.whole()).pieces.size(), std::size_t{3});
    SAME(text(one.bytes(498, 10)), doc.substr(498, 2) + "abcdef" + doc.substr(500, 2));
}

// Pieces merge at every seam. Typing one character at a time at the end and in the middle extends
// one piece of additions; a scrap taken from the start and put back merges with its right neighbor;
// joining scraps at the front and at the back merges adjacent pieces, and an empty scrap changes
// nothing. The text would be the same without merging, but the cost of everything depends on the
// number of pieces.
TRIAL("Quilt: pieces merge at the seams") {
    Quilt one{bytes("abcdef")};

    const auto pieces = [&one] { return one.taken(0, one.whole()).pieces.size(); };

    for (const char sign : std::string{"xyz"}) one.insert(one.whole(), std::string(1, sign));

    SAME(pieces(), std::size_t{2});

    for (const char sign : std::string{"123"}) one.insert(3 + static_cast<std::size_t>(sign - '1'), std::string(1, sign));

    SAME(whole(one), std::string{"abc123defxyz"});
    SAME(pieces(), std::size_t{4});

    Quilt other{bytes("abcdef")};

    const Quilt::Scrap head = other.taken(0, 3);

    other.erase(0, 3);
    other.put(0, head);

    SAME(other.taken(0, other.whole()).pieces.size(), std::size_t{1});

    Quilt::Scrap kept = other.taken(2, 1);

    Quilt::joined(kept, other.taken(1, 1), true);
    Quilt::joined(kept, other.taken(3, 2), false);

    SAME(kept.pieces.size(), std::size_t{1});
    SAME(other.text(kept), std::string{"bcde"});

    Quilt::joined(kept, Quilt::Scrap{}, true);
    Quilt::joined(kept, Quilt::Scrap{}, false);

    SAME(kept.pieces.size(), std::size_t{1});
    SAME(kept.much, std::size_t{4});

    // A scrap of two pieces whose ends go one into each neighbor leaves the neighbors side by side,
    // and they are sewn too: typing a character and taking it back, then cutting across that place
    // and undoing the cut, gives the one piece of the start.
    Quilt third{bytes("abcdef")};

    third.insert(3, "X");
    third.erase(3, 1);

    const Quilt::Scrap middle = third.taken(1, 4);

    SAME(middle.pieces.size(), std::size_t{2});

    third.erase(1, 4);
    third.put(1, middle);

    SAME(whole(third), std::string{"abcdef"});
    SAME(third.taken(0, third.whole()).pieces.size(), std::size_t{1});
}

// Thousands of pieces lie in many patches: random insertions cut the document into pieces of one
// byte, erasures and scraps then span patches, and a scrap of thousands of pieces goes back in one
// put, which divides the patch it lands in. After each change the quilt agrees with a plain string,
// and a scrap taken across patches gives the same text.
TRIAL("Quilt: changes across patches agree with the model") {
    std::mt19937 gen{11};

    std::string want(3000, '.');

    for (std::size_t at = 0; at < want.size(); ++at) want[at] = static_cast<char>('a' + at % 26);

    Quilt one{bytes(want)};

    for (int step = 0; step < 6000; ++step) {
        const std::size_t at = gen() % (want.size() + 1);
        const std::string what(1, static_cast<char>('A' + gen() % 26));

        want.insert(at, what);
        one.insert(at, what);
    }

    SAME(whole(one), want);
    OUGHT(one.taken(0, one.whole()).pieces.size() > 4096);

    for (int step = 0; step < 300; ++step) {
        const std::size_t kind = gen() % 4;

        if (kind == 0 && want.size() > 10) {
            const std::size_t at = gen() % want.size();
            const std::size_t much = 1 + gen() % std::min<std::size_t>(want.size() - at, 2500);

            want.erase(at, much);
            one.erase(at, much);
        } else if (kind == 1 && !want.empty()) {
            const std::size_t at = gen() % want.size();
            const std::size_t much = 1 + gen() % std::min<std::size_t>(want.size() - at, 3000);

            const Quilt::Scrap cut = one.taken(at, much);

            SAME(one.text(cut), want.substr(at, much));

            const std::string moved = want.substr(at, much);

            want.erase(at, much);
            one.erase(at, much);

            const std::size_t to = gen() % (want.size() + 1);

            want.insert(to, moved);
            one.put(to, cut);
        } else {
            const std::size_t at = gen() % (want.size() + 1);
            const std::string what(1 + gen() % 3, static_cast<char>('0' + gen() % 10));

            want.insert(at, what);
            one.insert(at, what);
        }

        SAME(one.whole(), want.size());

        if (step % 10 == 0) SAME(whole(one), want);
    }

    SAME(whole(one), want);

    const Quilt::Scrap all = one.taken(0, one.whole());

    one.erase(0, one.whole());

    SAME(one.whole(), std::size_t{0});
    SAME(whole(one), std::string{});

    one.put(0, all);

    SAME(whole(one), want);

    for (std::size_t at = 0; at + 4096 <= want.size(); at += 997) SAME(text(one.bytes(at, 4096)), want.substr(at, 4096));
}
