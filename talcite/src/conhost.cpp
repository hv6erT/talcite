#include "conhost.hpp"

#include "talcite/encoding.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace talcite::conhost {

namespace {

// Najdłuższa sekwencja CSI, najwięcej parametrów i najdłuższy napis OSC, jakie trzymamy. To, co
// pisze `Term`, mieści się w ułamku tego; dłuższe to śmieci i nie mają prawa zjeść pamięci.
constexpr std::size_t longest = 64;
constexpr std::size_t most = 32;
constexpr std::size_t told_most = 4096;

// Kolejność ANSI ma czerwień w bicie zerowym, konsola — błękit.
std::uint8_t console_of(int ansi) {
    return static_cast<std::uint8_t>(((ansi & 1) << 2) | (ansi & 2) | ((ansi & 4) >> 2) | (ansi & 8));
}

std::string hex(int component) {
    char room[8];

    std::snprintf(room, sizeof room, "%04x", component * 257);

    return room;
}

// Wirtualne klawisze i bity stanu z winuser.h i wincon.h — przepisane, bo ten plik nie ciągnie Win32.
constexpr std::uint16_t back = 0x08;
constexpr std::uint16_t tab = 0x09;
constexpr std::uint16_t enter = 0x0D;
constexpr std::uint16_t escape = 0x1B;
constexpr std::uint16_t space = 0x20;
constexpr std::uint16_t prior = 0x21;
constexpr std::uint16_t next = 0x22;
constexpr std::uint16_t end = 0x23;
constexpr std::uint16_t home = 0x24;
constexpr std::uint16_t left = 0x25;
constexpr std::uint16_t up = 0x26;
constexpr std::uint16_t right = 0x27;
constexpr std::uint16_t down = 0x28;
constexpr std::uint16_t insert = 0x2D;
constexpr std::uint16_t remove = 0x2E;
constexpr std::uint16_t f1 = 0x70;

constexpr std::uint32_t right_alt = 0x01;
constexpr std::uint32_t left_alt = 0x02;
constexpr std::uint32_t right_ctrl = 0x04;
constexpr std::uint32_t left_ctrl = 0x08;
constexpr std::uint32_t shift = 0x10;

constexpr std::uint32_t moved = 0x01;
constexpr std::uint32_t wheeled = 0x04;
constexpr std::uint32_t hwheeled = 0x08;

}

Palette classic() {
    return {{{0, 0, 0},
             {0, 0, 128},
             {0, 128, 0},
             {0, 128, 128},
             {128, 0, 0},
             {128, 0, 128},
             {128, 128, 0},
             {192, 192, 192},
             {128, 128, 128},
             {0, 0, 255},
             {0, 255, 0},
             {0, 255, 255},
             {255, 0, 0},
             {255, 0, 255},
             {255, 255, 0},
             {255, 255, 255}}};
}

std::uint8_t nearest(const Palette& among, int red, int green, int blue) {
    std::uint8_t best = 0;
    long closest = -1;

    for (std::size_t at = 0; at < among.size(); ++at) {
        const long r = among[at].red - red;
        const long g = among[at].green - green;
        const long b = among[at].blue - blue;
        const long far = r * r + g * g + b * b;

        if (closest < 0 || far < closest) {
            closest = far;
            best = static_cast<std::uint8_t>(at);
        }
    }

    return best;
}

Screen::Screen(ui::Size size, std::uint8_t ink, std::uint8_t paper, Palette palette)
    : palette_(palette), ink_(ink), paper_(paper) {
    pen_.ink = ink;
    pen_.paper = paper;

    resize(size);
}

void Screen::fed(std::string_view bytes) {
    for (const char one : bytes) taken(static_cast<unsigned char>(one));
}

// Nowa siatka z tym, co się zmieściło ze starej. Ekran zapasowy przycinamy razem z głównym, bo
// powrót z niego oddaje główny w nowym rozmiarze.
void Screen::resize(ui::Size size) {
    const ui::Size made{std::max(1, size.wide), std::max(1, size.high)};

    const auto moved = [&](const std::vector<Cell>& from) {
        std::vector<Cell> into(static_cast<std::size_t>(made.wide) * static_cast<std::size_t>(made.high), blank());

        if (from.empty()) return into;

        for (int y = 0; y < std::min(made.high, size_.high); ++y)
            for (int x = 0; x < std::min(made.wide, size_.wide); ++x)
                into[static_cast<std::size_t>(y * made.wide + x)] = from[static_cast<std::size_t>(y * size_.wide + x)];

        return into;
    };

    cells_ = moved(cells_);

    if (apart_) kept_ = moved(kept_);

    size_ = made;

    cursor_.x = std::min(cursor_.x, made.wide - 1);
    cursor_.y = std::min(cursor_.y, made.high - 1);
    kept_cursor_.x = std::min(kept_cursor_.x, made.wide - 1);
    kept_cursor_.y = std::min(kept_cursor_.y, made.high - 1);
    pending_ = false;

    dirty_.assign(static_cast<std::size_t>(made.high), true);
}

const Cell& Screen::at(int x, int y) const {
    static const Cell outside{};

    if (x < 0 || y < 0 || x >= size_.wide || y >= size_.high) return outside;

    return cells_[static_cast<std::size_t>(y * size_.wide + x)];
}

std::vector<int> Screen::dirty() {
    std::vector<int> made;

    for (std::size_t y = 0; y < dirty_.size(); ++y)
        if (dirty_[y]) made.push_back(static_cast<int>(y));

    std::fill(dirty_.begin(), dirty_.end(), false);

    return made;
}

std::string Screen::answers() {
    std::string made;

    made.swap(answers_);

    return made;
}

std::optional<std::string> Screen::title() {
    std::optional<std::string> made;

    made.swap(title_);

    return made;
}

void Screen::taken(unsigned char byte) {
    switch (mode_) {
        case Mode::ground: break;
        case Mode::escape: escaped(byte); return;
        case Mode::csi: gathered(byte); return;
        case Mode::text:
        case Mode::swallowed: stringed(byte); return;
        case Mode::ending:
            if (byte == '\\') {
                if (lead_osc_) told();

                mode_ = Mode::ground;

                return;
            }

            // Esc bez ukośnika przerywa napis i zaczyna nową sekwencję.
            mode_ = Mode::escape;
            escaped(byte);

            return;
    }

    // Urwany znak UTF-8 wychodzi jako zastępczy, a bajt, który go urwał, czytamy od nowa.
    if (left_ > 0) {
        if ((byte & 0xC0) == 0x80) {
            code_ = (code_ << 6) | (byte & 0x3F);

            if (--left_ == 0) put(code_);

            return;
        }

        left_ = 0;
        put(0xFFFD);
    }

    if (byte == 0x1B) {
        mode_ = Mode::escape;

        return;
    }

    if (byte < 0x20 || byte == 0x7F) {
        control(byte);

        return;
    }

    if (byte < 0x80) {
        put(byte);

        return;
    }

    const std::size_t more = utf8::trailing(byte);

    if (more == 0) {
        put(0xFFFD);

        return;
    }

    code_ = byte & (0x3F >> more);
    left_ = more;
}

void Screen::escaped(unsigned char byte) {
    switch (byte) {
        case '[':
            mode_ = Mode::csi;
            parameters_.clear();
            marker_ = 0;
            intermediate_ = 0;
            length_ = 0;

            return;
        case ']':
            mode_ = Mode::text;
            lead_osc_ = true;
            string_.clear();

            return;
        case 'P':
        case '_':
        case '^':
        case 'X':
            mode_ = Mode::swallowed;
            lead_osc_ = false;

            return;
        case 0x1B: return;
        default: mode_ = Mode::ground; return;
    }
}

// Za długa sekwencja nie wychodzi w tekst: połykamy ją do bajtu końcowego, jak xterm.
void Screen::gathered(unsigned char byte) {
    if (++length_ > longest) {
        if (byte == 0x1B) mode_ = Mode::escape;
        else if (byte >= 0x40 && byte <= 0x7E) mode_ = Mode::ground;

        return;
    }

    if (byte >= '0' && byte <= '9') {
        if (parameters_.empty()) parameters_.push_back(0);

        parameters_.back() = std::min(parameters_.back() * 10 + (byte - '0'), 99999);

        return;
    }

    if (byte == ';' || byte == ':') {
        if (parameters_.empty()) parameters_.push_back(0);
        if (parameters_.size() < most) parameters_.push_back(0);

        return;
    }

    if (byte >= 0x3C && byte <= 0x3F) {
        marker_ = static_cast<char>(byte);

        return;
    }

    if (byte >= 0x20 && byte <= 0x2F) {
        intermediate_ = static_cast<char>(byte);

        return;
    }

    if (byte >= 0x40 && byte <= 0x7E) {
        mode_ = Mode::ground;
        dispatched(static_cast<char>(byte));

        return;
    }

    if (byte == 0x1B) mode_ = Mode::escape;
}

void Screen::stringed(unsigned char byte) {
    if (byte == 0x1B) {
        mode_ = Mode::ending;

        return;
    }

    if (mode_ != Mode::text) return;

    if (byte == 0x07) {
        told();
        mode_ = Mode::ground;

        return;
    }

    if (string_.size() < told_most) string_ += static_cast<char>(byte);
}

// OSC: tytuł (0 i 2) i pytanie o tło (11). Resztę połykamy.
void Screen::told() {
    const std::size_t semi = string_.find(';');

    if (semi == std::string::npos) return;

    const std::string_view which{string_.data(), semi};
    const std::string_view said{string_.data() + semi + 1, string_.size() - semi - 1};

    if (which == "0" || which == "2") title_ = std::string{said};

    if (which == "11" && said == "?") {
        const Rgb& paper = palette_[paper_ & 15];

        answers_ += "\x1b]11;rgb:" + hex(paper.red) + "/" + hex(paper.green) + "/" + hex(paper.blue) + "\x1b\\";
    }
}

int Screen::parameter(std::size_t which, int fallback) const {
    return which < parameters_.size() && parameters_[which] != 0 ? parameters_[which] : fallback;
}

void Screen::dispatched(char last) {
    if (marker_ == '?') {
        if (intermediate_ == '$' && last == 'p') asked_mode();
        else if (intermediate_ == 0 && (last == 'h' || last == 'l')) moded(last == 'h');

        return;
    }

    // Wtórne DA i reszta ze znacznikiem: milczymy, jak terminal, który pytania nie zna.
    if (marker_ != 0 || intermediate_ != 0) return;

    switch (last) {
        case 'H':
        case 'f':
            cursor_.y = std::clamp(parameter(0, 1) - 1, 0, size_.high - 1);
            cursor_.x = std::clamp(parameter(1, 1) - 1, 0, size_.wide - 1);
            pending_ = false;

            return;
        case 'J': erased_screen(parameter(0, 0)); return;
        case 'K': erased_line(parameter(0, 0)); return;
        case 'm': styled(); return;
        case 'n':
            if (parameter(0, 0) == 6)
                answers_ += "\x1b[" + std::to_string(cursor_.y + 1) + ";" + std::to_string(cursor_.x + 1) + "R";

            return;
        case 'c':
            // Pierwotne DA: VT220 z kolorem ANSI. Bez czwórki, bo sixela nie ma.
            answers_ += "\x1b[?62;22c";

            return;
        case 't':
            if (parameter(0, 0) == 16 && pixels_.has_value())
                answers_ += "\x1b[6;" + std::to_string(pixels_->high) + ";" + std::to_string(pixels_->wide) + "t";

            return;
        default: return;
    }
}

void Screen::moded(bool on) {
    for (const int one : parameters_) switch (one) {
            case 25: shown_ = on; break;
            case 7:
                wrapping_ = on;
                pending_ = false;
                break;
            case 47: switched(on, false); break;
            case 1049: switched(on, true); break;
            case 1000: tracking_.clicks = on; break;
            case 1002: tracking_.drags = on; break;
            case 1003: tracking_.moves = on; break;
            case 1006: tracking_.sgr = on; break;
            default: break;
        }
}

// DECRQM: tryby, które tłumaczenie naprawdę obsługuje, odpowiadają stanem (1 — włączony,
// 2 — wyłączony); reszta zerem, czyli „nie znam". Wklejania w nawiasach, ogniska i synchronizacji
// nie ma: wklejka przychodzi z konsoli zwykłymi klawiszami, a rysujemy i tak całą klatką naraz.
void Screen::asked_mode() {
    const int which = parameter(0, 0);

    int state = 0;

    switch (which) {
        case 25: state = shown_ ? 1 : 2; break;
        case 7: state = wrapping_ ? 1 : 2; break;
        case 47:
        case 1049: state = apart_ ? 1 : 2; break;
        case 1000: state = tracking_.clicks ? 1 : 2; break;
        case 1002: state = tracking_.drags ? 1 : 2; break;
        case 1003: state = tracking_.moves ? 1 : 2; break;
        case 1006: state = tracking_.sgr ? 1 : 2; break;
        default: break;
    }

    answers_ += "\x1b[?" + std::to_string(which) + ";" + std::to_string(state) + "$y";
}

// **Ekran zapasowy.** Klej trzyma pod nim osobny bufor konsoli, więc wejście czyści siatkę i każe
// przepisać wszystko, a wyjście oddaje siatkę główną **bez** znaczenia wierszy: bufor główny
// konsoli stoi nietknięty, a wiersze zmienione na zapasowym nie są jego.
void Screen::switched(bool on, bool cursor) {
    if (on == apart_) return;

    if (on) {
        kept_ = cells_;
        kept_cursor_ = cursor_;

        std::fill(cells_.begin(), cells_.end(), blank());
        std::fill(dirty_.begin(), dirty_.end(), true);

        apart_ = true;

        return;
    }

    cells_ = std::move(kept_);
    kept_.clear();

    if (cursor) cursor_ = kept_cursor_;

    pending_ = false;
    std::fill(dirty_.begin(), dirty_.end(), false);

    apart_ = false;
}

void Screen::styled() {
    if (parameters_.empty()) parameters_.push_back(0);

    for (std::size_t at = 0; at < parameters_.size(); ++at) {
        const int one = parameters_[at];

        if (one == 0) pen_ = Pen{ink_, paper_};
        else if (one == 1) pen_.bold = true;
        else if (one == 22) pen_.bold = false;
        else if (one == 4) pen_.under = true;
        else if (one == 24) pen_.under = false;
        else if (one == 7) pen_.reverse = true;
        else if (one == 27) pen_.reverse = false;
        else if (one >= 30 && one <= 37) pen_.ink = console_of(one - 30);
        else if (one == 39) pen_.ink = ink_;
        else if (one >= 40 && one <= 47) pen_.paper = console_of(one - 40);
        else if (one == 49) pen_.paper = paper_;
        else if (one >= 90 && one <= 97) pen_.ink = console_of(one - 90 + 8);
        else if (one >= 100 && one <= 107) pen_.paper = console_of(one - 100 + 8);
        else if (one == 38 || one == 48) colored(at, one == 38);
    }
}

// 38;5;n i 38;2;r;g;b. Pierwsza szesnastka palety to kolory ANSI wprost; sześcian i szarości idą
// do najbliższego koloru konsoli.
void Screen::colored(std::size_t& at, bool foreground) {
    if (at + 1 >= parameters_.size()) return;

    std::optional<std::uint8_t> made;

    if (parameters_[at + 1] == 5 && at + 2 < parameters_.size()) {
        const int which = parameters_[at + 2];

        if (which < 16) made = console_of(which);
        else if (which < 232) {
            static constexpr int levels[6] = {0, 95, 135, 175, 215, 255};

            const int cube = which - 16;

            made = nearest(palette_, levels[cube / 36], levels[(cube / 6) % 6], levels[cube % 6]);
        } else if (which < 256) {
            const int gray = 8 + 10 * (which - 232);

            made = nearest(palette_, gray, gray, gray);
        }

        at += 2;
    } else if (parameters_[at + 1] == 2 && at + 4 < parameters_.size()) {
        made = nearest(palette_, parameters_[at + 2], parameters_[at + 3], parameters_[at + 4]);

        at += 4;
    } else {
        return;
    }

    if (!made.has_value()) return;

    if (foreground) pen_.ink = *made;
    else pen_.paper = *made;
}

// Kolory komórki z pióra. Wytłuszczenie konsola zna tylko jako jasność, więc rozjaśnia ciemną barwę.
Cell Screen::inked(char32_t glyph) const {
    Cell made;

    made.glyph = glyph;
    made.ink = pen_.bold && pen_.ink < 8 ? static_cast<std::uint8_t>(pen_.ink | 8) : pen_.ink;
    made.paper = pen_.paper;
    made.under = pen_.under;

    if (pen_.reverse) std::swap(made.ink, made.paper);

    return made;
}

Cell Screen::blank() const {
    Cell made = inked(U' ');

    made.under = false;

    return made;
}

Cell& Screen::cell(int x, int y) {
    return cells_[static_cast<std::size_t>(y * size_.wide + x)];
}

// **Szerokość mierzymy tak jak `Term`** — inaczej klatka i siatka rozjechałyby się po pierwszym
// znaku chińskim. Wyjątkiem jest wszystko spoza BMP: komórka konsoli trzyma jedną jednostkę UTF-16,
// więc taki znak staje jako „?" na dwóch kratkach, tak jak conhost bez VT stawiał parę zastępczą.
// Dzięki temu sonda dowiaduje się, że sekstantów i oktantów tu nie ma.
//
// Znak na dwie kratki: lewa niesie znak, prawa jest pusta. Starszy conhost poza stronami kodowymi
// CJK nie rysuje znaku szerzej niż na kratkę, więc druga kratka zostaje odstępem — układ się zgadza
// i nic nie wjeżdża na sąsiada.
void Screen::put(char32_t one) {
    if (one >= 0x80 && one <= 0x9F) return;

    const bool beyond = one > 0xFFFF;
    const int much = beyond ? 2 : width(one);
    const char32_t glyph = beyond || one == 0xFFFD ? U'?' : one;

    if (pending_) {
        pending_ = false;
        cursor_.x = 0;
        lined();
    }

    if (much == 2 && cursor_.x >= size_.wide - 1) {
        if (!wrapping_ || size_.wide < 2) return;

        cursor_.x = 0;
        lined();
    }

    written(cursor_.x, cursor_.y, inked(glyph));

    if (much == 2) {
        Cell half = inked(U' ');
        half.trailing = true;

        written(cursor_.x + 1, cursor_.y, half);
    }

    cursor_.x += much;

    if (cursor_.x >= size_.wide) {
        cursor_.x = size_.wide - 1;
        pending_ = wrapping_;
    }
}

// Zapis do komórki rozbija znak na dwie kratki, którego połowę przykrywa: druga połowa staje pusta.
void Screen::written(int x, int y, const Cell& one) {
    Cell& here = cell(x, y);

    if (here.trailing && !one.trailing && x > 0) cell(x - 1, y).glyph = U' ';

    if (x + 1 < size_.wide) {
        Cell& right = cell(x + 1, y);

        if (right.trailing) {
            right.trailing = false;
            right.glyph = U' ';
        }
    }

    here = one;

    touched(y);
}

void Screen::touched(int y) { dirty_[static_cast<std::size_t>(y)] = true; }

void Screen::control(unsigned char one) {
    switch (one) {
        case '\r':
            cursor_.x = 0;
            pending_ = false;
            break;
        case '\n':
        case 0x0B:
        case 0x0C: lined(); break;
        case '\b':
            if (cursor_.x > 0) --cursor_.x;
            pending_ = false;
            break;
        case '\t':
            cursor_.x = std::min(size_.wide - 1, (cursor_.x / 8 + 1) * 8);
            pending_ = false;
            break;
        default: break;
    }
}

// Nowy wiersz; na dole siatka jedzie w górę o jeden.
void Screen::lined() {
    pending_ = false;

    if (cursor_.y + 1 < size_.high) {
        ++cursor_.y;

        return;
    }

    std::move(cells_.begin() + size_.wide, cells_.end(), cells_.begin());
    std::fill(cells_.end() - size_.wide, cells_.end(), blank());
    std::fill(dirty_.begin(), dirty_.end(), true);
}

void Screen::cleared(int x, int y, int much) {
    for (int at = x; at < x + much && at < size_.wide; ++at) written(at, y, blank());
}

void Screen::erased_line(int how) {
    pending_ = false;

    if (how == 0) cleared(cursor_.x, cursor_.y, size_.wide - cursor_.x);
    else if (how == 1) cleared(0, cursor_.y, cursor_.x + 1);
    else if (how == 2) cleared(0, cursor_.y, size_.wide);
}

void Screen::erased_screen(int how) {
    pending_ = false;

    if (how == 0) {
        cleared(cursor_.x, cursor_.y, size_.wide - cursor_.x);

        for (int y = cursor_.y + 1; y < size_.high; ++y) cleared(0, y, size_.wide);
    } else if (how == 1) {
        for (int y = 0; y < cursor_.y; ++y) cleared(0, y, size_.wide);

        cleared(0, cursor_.y, cursor_.x + 1);
    } else if (how == 2) {
        for (int y = 0; y < size_.high; ++y) cleared(0, y, size_.wide);
    }
}

// Klawisze nazwane idą tak, jak wysyła je conhost w trybie VT i xterm: strzałki, Home i End jako
// CSI z literą, reszta z tyldą, F1–F4 jako SS3, modyfikatory jako drugi parametr. Znak z Altem
// dostaje przedrostek Esc — chyba że Alt szedł z Ctrl i dał zwykły znak: to AltGr (Windows zgłasza
// go jako prawy Alt z lewym Ctrl), czyli po prostu litera z układu, jak „ą".
std::string typed(const Pressed& one, char16_t& pending) {
    const bool ctrl = (one.state & (left_ctrl | right_ctrl)) != 0;
    const bool alt = (one.state & (left_alt | right_alt)) != 0;
    const bool shifted = (one.state & shift) != 0;

    const int mods = 1 + (shifted ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    const std::string prefix = alt ? "\x1b" : "";

    const auto lettered = [&](char last) {
        return mods > 1 ? "\x1b[1;" + std::to_string(mods) + last : std::string{"\x1b["} + last;
    };

    const auto tilded = [&](int code) {
        return "\x1b[" + std::to_string(code) + (mods > 1 ? ";" + std::to_string(mods) : std::string{}) + "~";
    };

    std::string made;

    switch (one.key) {
        case up: made = lettered('A'); break;
        case down: made = lettered('B'); break;
        case right: made = lettered('C'); break;
        case left: made = lettered('D'); break;
        case home: made = lettered('H'); break;
        case end: made = lettered('F'); break;
        case insert: made = tilded(2); break;
        case remove: made = tilded(3); break;
        case prior: made = tilded(5); break;
        case next: made = tilded(6); break;
        case f1:
        case f1 + 1:
        case f1 + 2:
        case f1 + 3: {
            const char last = static_cast<char>('P' + (one.key - f1));

            made = mods > 1 ? lettered(last) : std::string{"\x1bO"} + last;

            break;
        }
        case f1 + 4: made = tilded(15); break;
        case f1 + 5: made = tilded(17); break;
        case f1 + 6: made = tilded(18); break;
        case f1 + 7: made = tilded(19); break;
        case f1 + 8: made = tilded(20); break;
        case f1 + 9: made = tilded(21); break;
        case f1 + 10: made = tilded(23); break;
        case f1 + 11: made = tilded(24); break;
        case enter: made = prefix + "\r"; break;
        case tab: made = shifted ? std::string{"\x1b[Z"} : prefix + "\t"; break;
        case back: made = prefix + (ctrl ? "\x08" : "\x7f"); break;
        case escape: made = prefix + "\x1b"; break;
        default: {
            char32_t code = one.unit;

            if (code == 0) {
                if (ctrl && one.key == space) code = 0;
                else if (ctrl && alt && one.key >= 'A' && one.key <= 'Z') code = one.key - 'A' + 1;
                else return {};

                made = prefix + std::string(1, static_cast<char>(code));

                break;
            }

            if (code >= 0xD800 && code <= 0xDBFF) {
                pending = one.unit;

                return {};
            }

            if (code >= 0xDC00 && code <= 0xDFFF) {
                if (pending == 0) return {};

                code = 0x10000 + ((static_cast<char32_t>(pending) - 0xD800) << 10) + (code - 0xDC00);
            }

            pending = 0;

            made = (alt && !(ctrl && code >= 0x20) ? "\x1b" : "") + utf8::encode(code);

            break;
        }
    }

    std::string repeated;

    for (int again = 0; again < std::max(1, one.repeat); ++again) repeated += made;

    return repeated;
}

// Konsola mówi, które przyciski są wciśnięte teraz; dekoder chce wiedzieć, który się wcisnął albo
// puścił. Stąd pamięć poprzedniego stanu — aktualizowana także wtedy, gdy aplikacja myszy nie chce:
// przycisk wciśnięty przed włączeniem śledzenia nie wraca potem jako świeże wciśnięcie, tylko jako
// przeciąganie, a jego puszczenie przychodzi, tak jak w xtermie.
std::string Pointing::pointed(const Pointed& one, const Tracking& wanted) {
    const bool reporting = (wanted.clicks || wanted.drags || wanted.moves) && wanted.sgr;

    int mods = 0;

    if ((one.state & shift) != 0) mods |= 4;
    if ((one.state & (left_alt | right_alt)) != 0) mods |= 8;
    if ((one.state & (left_ctrl | right_ctrl)) != 0) mods |= 16;

    const auto said = [&](int button, bool released) {
        return "\x1b[<" + std::to_string(button | mods) + ";" + std::to_string(one.at.x + 1) + ";" +
               std::to_string(one.at.y + 1) + (released ? "m" : "M");
    };

    if ((one.flags & wheeled) != 0) {
        if (!reporting) return {};

        const auto delta = static_cast<std::int16_t>(one.buttons >> 16);

        return said(delta > 0 ? 64 : 65, false);
    }

    if ((one.flags & hwheeled) != 0) return {};

    const std::uint32_t now = one.buttons & 0x7;
    const std::uint32_t was = held_;

    held_ = now;

    if (!reporting) return {};

    // Bity konsoli: lewy, prawy, środkowy; numery xterma: lewy 0, środkowy 1, prawy 2.
    static constexpr std::pair<std::uint32_t, int> buttons[3] = {{0x1, 0}, {0x4, 1}, {0x2, 2}};

    std::string made;

    for (const auto& [bit, code] : buttons) {
        if ((now & bit) != 0 && (was & bit) == 0) made += said(code, false);
        if ((now & bit) == 0 && (was & bit) != 0) made += said(code, true);
    }

    if (!made.empty() || (one.flags & moved) == 0) return made;

    if (now != 0 && (wanted.drags || wanted.moves)) {
        for (const auto& [bit, code] : buttons)
            if ((now & bit) != 0) return said(32 + code, false);
    }

    if (now == 0 && wanted.moves) return said(32 + 3, false);

    return made;
}

}
