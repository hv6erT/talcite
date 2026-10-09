#include "talcite/span.hpp"

#include "talcite/skin.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"

#include "eliding.hpp"

namespace talcite::ui {

Span::Span(std::string text, SpanShape shape) : text_(std::move(text)), shape_(shape) {
    role("talcite.normal");
}

// Napis nie ma czego trzymać w środku.
int Span::capacity() const { return 0; }

namespace {

// **Twardy koniec wiersza dzieli zawsze**, także bez zawijania. Wcześniej `\n` szedł do kartki
// jak każdy inny bajt i wychodził z niego znak zastępczy w środku zdania — a komunikaty z dwóch
// zdań pisze się właśnie tak. Dzielenie daje o jeden kawałek więcej, niż jest końców: „a\nb" to
// dwa wiersze, a „a\n" to wiersz i pusty wiersz po nim. Reguła bez wyjątków czyta się lepiej niż
// zgadywanie, czy autor chciał tego pustego.
//
// Powrót karetki zdejmujemy z końca kawałka, bo tekst spod Windows niesie „\r\n" i sam „\r"
// zostawiony w napisie wyszedłby na ekranie jako śmieć.
std::vector<std::string> parted(const std::string& what) {
    std::vector<std::string> made;

    for (std::size_t at = 0;;) {
        const std::size_t end = what.find('\n', at);
        const std::size_t upto = end == std::string::npos ? what.size() : end;

        std::string one = what.substr(at, upto - at);

        if (!one.empty() && one.back() == '\r') one.pop_back();

        made.push_back(std::move(one));

        if (end == std::string::npos) break;

        at = end + 1;
    }

    return made;
}

// Z zawijaniem łamiemy po spacjach, a słowo dłuższe niż wiersz — w miejscu, bo jedyną alternatywą
// byłoby wyjechanie poza kartkę, a tego View i tak nie wypuści. **Każdy akapit zawija się osobno**,
// więc twardy koniec wiersza nigdy się nie skleja z sąsiednim zdaniem.
//
// Funkcja plikowa, a nie metoda: nie sięga do niczego, co należy do napisu, a nagłówek publiczny
// nie ma powodu wiedzieć, że w środku jest jeszcze jeden krok.
void wrap_into(const std::string& what, int wide, std::vector<std::string>& into) {
    std::vector<std::string> words;

    for (std::size_t at = 0; at <= what.size();) {
        const std::size_t space = what.find(' ', at);
        const std::size_t upto = space == std::string::npos ? what.size() : space;

        if (upto > at) words.push_back(what.substr(at, upto - at));

        if (space == std::string::npos) break;

        at = space + 1;
    }

    std::string line;
    int much = 0;

    for (std::string word : words) {
        int room = measured(word).wide;

        // Słowo dłuższe niż cały wiersz urywamy tyle razy, ile trzeba. Jeśli nie mieści się nawet
        // jeden znak — bo wiersz ma jedną kratkę, a znak jest podwójny — zostawiamy je w całości
        // i przytnie je kartka; udawanie, że coś się zmieściło, byłoby gorsze.
        while (room > wide) {
            if (!line.empty()) {
                into.push_back(std::move(line));

                line.clear();
                much = 0;
            }

            std::string piece = cut(word, wide);

            if (piece.empty()) break;

            word.erase(0, piece.size());

            into.push_back(std::move(piece));

            room = measured(word).wide;
        }

        if (word.empty()) continue;

        if (line.empty()) {
            line = std::move(word);
            much = room;
        } else if (much + 1 + room <= wide) {
            line += ' ';
            line += word;
            much += 1 + room;
        } else {
            into.push_back(std::move(line));

            line = std::move(word);
            much = room;
        }
    }

    if (!line.empty()) into.push_back(std::move(line));
}

}

// Akapit pusty zostaje pustym wierszem, a nie znika: to jest odstęp, który ktoś napisał świadomie.
std::vector<std::string> Span::lines(int wide) const {
    if (text_.empty()) return {};

    const std::vector<std::string> parts = parted(text_);

    if (!shape_.wrapped) return parts;

    if (wide <= 0) return {};

    std::vector<std::string> made;

    for (const std::string& one : parts) {
        if (one.empty()) {
            made.emplace_back();

            continue;
        }

        wrap_into(one, wide, made);
    }

    return made;
}

// Wysokość liczy się teraz z wierszy, nie z zawijania — bo wierszy bywa więcej także bez niego.
Size Span::wants(Size room) const {
    if (text_.empty()) return {};

    const std::vector<std::string> made = lines(room.wide);

    Size want{0, static_cast<int>(made.size())};

    for (const std::string& one : made) want.wide = std::max(want.wide, measured(one).wide);

    return want;
}

// Skracanie i wyrównanie robimy dopiero tutaj, przy rysowaniu. wants mówi, ile napis chce
// naprawdę — gdyby oddawało długość po skróceniu, rodzic nigdy by się nie dowiedział, że jest za
// ciasno, i skrócony napis zostałby skrócony na zawsze.
View Span::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    // **Rdzeń nazwy roli zadaje się z zewnątrz.** Domyślnie `talcite.normal`, więc nic się nie
    // zmienia; ale napis postawiony w czerwonym pudełku błędu ma być czerwony, a jedna rola dla
    // wszystkich napisów w programie nie umie być naraz czerwona i granatowa. Kto buduje takie
    // pudełko, ten wie, co w nim stoi, i mówi to wprost.
    const Style look = how.style(role());
    const char32_t mark = how.glyph("talcite.elide");

    const std::vector<std::string> said = lines(given.wide);

    int y = 0;

    for (const std::string& one : said) {
        if (y >= given.high || y - seen.at.y >= seen.size.high) break;

        // Wiersz ponad oknem pomijamy bez rysowania — przy przewiniętym napisie to jest
        // różnica między dwudziestoma wierszami a wszystkimi.
        if (y < seen.at.y) {
            ++y;

            continue;
        }

        const std::string shown = elided(one, given.wide, shape_.elide, mark);

        int x = 0;

        if (shape_.justify != Justify::left) {
            const int room = given.wide - measured(shown).wide;

            x = shape_.justify == Justify::right ? room : room / 2;

            if (x < 0) x = 0;
        }

        made.put({x - seen.at.x, y - seen.at.y}, shown, look);

        ++y;
    }

    return made;
}

}
