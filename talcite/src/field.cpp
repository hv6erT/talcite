#include "talcite/field.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "placing.hpp"

namespace talcite::ui {

namespace {

// Krok w tył po UTF-8: schodzimy z bajtów ciągłych, aż staniemy na początku znaku. W przód robi to
// `letter`, w tył trzeba samemu — kodowanie czyta się tylko w jedną stronę.
std::size_t before(const std::string& text, std::size_t at) {
    if (at == 0) return 0;

    --at;

    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;

    return at;
}

std::size_t after(const std::string& text, std::size_t at) {
    if (at >= text.size()) return text.size();

    const std::size_t began = at;

    (void)letter(text, at);

    return at > began ? at : text.size();
}

// Ile kratek zajmuje kawałek napisu. Liczymy **szerokościami**, nie bajtami i nie znakami —
// inaczej pierwszy znak dwukolumnowy rozjechałby i kursor, i przewijanie.
int spanned(const std::string& text, std::size_t from, std::size_t upto) {
    int made = 0;

    std::size_t at = from;

    while (at < upto && at < text.size()) {
        const std::size_t began = at;

        made += widely(letter(text, at));

        if (began == at) break;
    }

    return made;
}

}

Field::Field(int wide, std::string text) : text_(std::move(text)), wide_(wide) {
    role("talcite.input");

    at_ = text_.size();
}

int Field::takes() const { return 0; }

bool Field::focusable() const { return able_; }

Size Field::wants(Size room) const {
    return {std::min(std::max(wide_, 0), room.wide), std::min(1, room.high)};
}

// Pole jest jedynym widżetem, który ma co powiedzieć o kursorze — i mówi to **we własnych
// współrzędnych**, bez oglądania się na widoczny kawałek: od przycięcia jest korzeń. Odpowiedź
// jest prawdziwa dopiero po złożeniu klatki, bo `from_` — czyli o ile treść jest przewinięta —
// wylicza się w `paint`, gdy znana jest szerokość, którą pole naprawdę dostało.
std::optional<Spot> Field::caret() const {
    if (!focused()) return {};

    return Spot{spanned(text_, from_, at_), 0};
}

// Wyłączenie oddaje ognisko najbliższemu chętnemu przodkowi — tak samo i z tego samego powodu, co
// w przycisku: bez ogniska dialog przestaje połykać klawisze.
void Field::able(bool given) {
    able_ = given;

    if (given) return;

    unfocus();
}

std::size_t Field::cursor() const {
    std::size_t made = 0;
    std::size_t at = 0;

    while (at < at_ && at < text_.size()) {
        const std::size_t began = at;

        (void)letter(text_, at);

        if (began == at) break;

        ++made;
    }

    return made;
}

// Na zewnątrz kursor liczy się **w znakach**, w środku w bajtach. Wołający nie ma powodu wiedzieć
// ani o UTF-8, ani o kratkach; obie te miary są tu i tak potrzebne, ale należą do pola.
void Field::cursor(std::size_t given) {
    at_ = 0;

    for (std::size_t which = 0; which < given && at_ < text_.size(); ++which)
        at_ = after(text_, at_);

    guessing();
}

void Field::text(std::string given) {
    text_ = std::move(given);
    at_ = text_.size();
    from_ = 0;

    guessing();
}

void Field::guess(std::function<std::string(const std::string& what)> given) {
    guess_ = std::move(given);

    guessing();
}

// Podpowiedź liczy się **przy zmianie treści, nie przy malowaniu**. Wołający ma prawo szukać jej
// w katalogu albo w historii, a to potrafi kosztować — malowanie dzieje się przy każdej klatce
// i nie jest miejscem na taką pracę.
//
// Podpowiadamy tylko, gdy kursor stoi **na końcu**: w środku napisu dopisek za kursorem byłby
// kłamstwem o tym, co się stanie po naciśnięciu klawisza. Podpowiedź, która nie zaczyna się od
// tego, co już napisano, jest odrzucana — nie ma jak jej dopisać.
void Field::guessing() {
    ahead_.clear();

    if (!guess_ || at_ != text_.size()) return;

    const std::string whole = guess_(text_);

    if (whole.size() <= text_.size()) return;

    if (whole.compare(0, text_.size(), text_) != 0) return;

    ahead_ = whole.substr(text_.size());
}

// Co wolno wpisać. **Pole liczbowe to jedna lambda, a nie osobny widżet** — Midnight Commander nie
// ma widżetu liczbowego i liczby wpisuje w zwykłe `input`, więc nie ma czego naśladować. Ta sama
// furtka służy do zakazu ukośników w nazwie pliku czy do samych cyfr szesnastkowych.
void Field::allows(std::function<bool(char32_t one)> given) { allows_ = std::move(given); }

void Field::insert(const std::string& what) {
    std::string clean;

    clean.reserve(what.size());

    std::size_t at = 0;

    while (at < what.size()) {
        const std::size_t began = at;
        const char32_t one = letter(what, at);

        if (began == at) break;

        // Pole jest jednowierszowe, więc końce wierszy z wklejenia zamieniamy na odstępy zamiast
        // wpuszczać do napisu znak, którego nie umiemy narysować.
        const char32_t which = (one == U'\n' || one == U'\r') ? U' ' : one;

        // Odsiew działa **i przy pisaniu, i przy wklejaniu**: wklejone „12ab34" w polu na cyfry
        // zostawia „1234", zamiast odrzucać całość. Odrzucenie wszystkiego karałoby za jeden zły
        // znak w środku, a przepuszczenie kłamałoby o tym, co pole przyjmuje.
        if (allows_ && !allows_(which)) continue;

        spelled(which, clean);
    }

    if (clean.empty()) return;

    text_.insert(at_, clean);
    at_ += clean.size();

    guessing();
}

View Field::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — ta sama reguła i ten sam powód co w przycisku.
    made.bare({{0, 0}, seen.size});

    const int room = std::max(given.wide, 0);

    // **Przewinięcie liczy się tutaj**, bo dopiero tu znana jest szerokość, którą pole naprawdę
    // dostało — rodzic ma prawo dać mniej, niż prosiło. To jedyne miejsce, gdzie malowanie zmienia
    // stan, i dzieje się tak świadomie: inaczej kursor potrafiłby wyjechać poza widoczny kawałek.
    if (at_ < from_) from_ = at_;

    while (room > 0 && spanned(text_, from_, at_) >= room) from_ = after(text_, from_);

    const Skin& how = skin();

    // **Podkreślenie nie jest już tutaj** — siedzi we wbudowanej skórce, w roli `talcite.input`.
    // W Midnight Commanderze pole wyróżnia sam kolor tła, więc gdy dojdą barwy, wystarczy podmienić
    // wpis w skórce i z kodu nie ubędzie ani nie przybędzie ani jedna linijka.
    const Style look = how.get<Style>(able_ ? role() : role() + ".off");

    made.fill({{-seen.at.x, -seen.at.y}, {room, 1}}, look);

    // Widoczny kawałek treści, przycięty do szerokości.
    std::size_t upto = from_;
    int wide = 0;

    while (upto < text_.size()) {
        const std::size_t began = upto;
        const std::size_t next = after(text_, upto);
        const int much = spanned(text_, began, next);

        if (wide + much > room) break;

        wide += much;
        upto = next;

        if (began == upto) break;
    }

    made.string({-seen.at.x, -seen.at.y}, text_.substr(from_, upto - from_), look);

    // Podpowiedź **wyszarzona**, za treścią, na tyle, ile zostało miejsca. Nie jest częścią napisu
    // i nie wchodzi do `text()` — dopóki nikt jej nie przyjmie.
    if (!ahead_.empty() && wide < room) {
        const Style dim = Style::merge(look, how.get<Style>(role() + ".guess"));

        made.string({wide - seen.at.x, -seen.at.y}, ahead_, dim);
    }

    return made;
}

// Enter, Esc i klawisze funkcyjne **oddajemy dalej** — to dzięki temu Enter w polu zatwierdza
// dialog, a Esc go zamyka, jak w Midnight Commanderze. Zwykłe litery bierzemy **zawsze**: bez tego
// „t" w polu naciskałoby przycisk, który ma tę literę w klawiszach aktywnych.
//
// Tab bierzemy **tylko wtedy, gdy jest co uzupełnić**. W innym razie oddajemy go dalej i ognisko
// przechodzi do następnego — tak samo rozstrzygają to powłoki.
bool Field::react(KeyEvent& what) {
    if (!able_) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    switch (what.key().code()) {
        // **Taba nie bierzemy.** Dobierał całą podpowiedź, ale tylko wtedy, gdy podpowiedź była —
        // więc ten sam klawisz raz przechodził do następnego widżetu, a raz nie, zależnie od
        // treści pola. Klawisz wędrówki nie może znikać zależnie od zawartości. Dobieranie
        // zostaje pod strzałką w prawo, która jest ruchem po własnej treści i niczego nie zabiera.

        case Code::right:
            // Na końcu treści strzałka w prawo **dobiera jedną literę podpowiedzi**.
            if (at_ == text_.size() && !ahead_.empty()) {
                const std::size_t much = after(ahead_, 0);

                text_ += ahead_.substr(0, much);
                at_ = text_.size();

                guessing();

                return true;
            }

            at_ = after(text_, at_);

            guessing();

            return true;

        case Code::left: at_ = before(text_, at_); guessing(); return true;

        case Code::home: at_ = 0; guessing(); return true;

        case Code::end: at_ = text_.size(); guessing(); return true;

        case Code::backspace: {
            if (at_ == 0) return true;

            const std::size_t began = before(text_, at_);

            text_.erase(began, at_ - began);
            at_ = began;

            guessing();

            return true;
        }

        case Code::remove: {
            if (at_ >= text_.size()) return true;

            text_.erase(at_, after(text_, at_) - at_);

            guessing();

            return true;
        }

        default: break;
    }

    const char32_t one = what.key().letter();

    if (one == 0) return false;

    std::string said;

    spelled(one, said);

    // Znak odsiany **połykamy**, a nie oddajemy wyżej. Klawisz był skierowany do pola i to, że pole
    // go nie chce, nie znaczy, że ma go dostać ktoś inny — inaczej „t" odrzucone w polu na cyfry
    // naciskałoby w dialogu przycisk „Tak".
    insert(said);

    return true;
}

bool Field::react(PasteEvent& what) {
    if (!able_) return false;

    insert(what.said());

    return true;
}

// Klik stawia kursor tam, gdzie kliknięto, i bierze ognisko. Kolumnę przeliczamy tą samą miarą,
// którą maluje `paint` — inaczej kursor lądowałby obok miejsca, w które celowano.
bool Field::react(ClickEvent& what) {
    if (!able_) return true;

    if (root() != nullptr) focus();

    const int wanted = own(*this, what.where()).x;

    std::size_t at = from_;
    int wide = 0;

    while (at < text_.size()) {
        const std::size_t next = after(text_, at);
        const int much = spanned(text_, at, next);

        if (wide + much > wanted) break;

        // Bez postępu przerywamy — ten sam strażnik, co przy każdym innym przejściu po napisie.
        // Wcześniej stało tu porównanie `next == at` **po** przypisaniu, czyli warunek zawsze
        // prawdziwy, a właściwe zabezpieczenie nie istniało.
        if (next == at) break;

        wide += much;
        at = next;
    }

    at_ = at;

    guessing();

    return true;
}

}
