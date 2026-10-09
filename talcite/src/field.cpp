#include "talcite/field.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"


namespace talcite::ui {

namespace {

// Krok w tył po UTF-8: schodzimy z bajtów ciągłych, aż staniemy na początku znaku. W przód robi to
// `utf8::decode`, w tył trzeba samemu — kodowanie czyta się tylko w jedną stronę.
std::size_t before(const std::string& text, std::size_t at) {
    if (at == 0) return 0;

    --at;

    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;

    return at;
}

std::size_t after(const std::string& text, std::size_t at) {
    if (at >= text.size()) return text.size();

    const std::size_t began = at;

    (void)utf8::decode(text, at);

    return at > began ? at : text.size();
}

}

// Ile kratek zajmuje kawałek napisu. Liczymy **szerokościami**, nie bajtami i nie znakami —
// inaczej pierwszy znak dwukolumnowy rozjechałby i kursor, i przewijanie. Pole ukryte rysuje
// każdy znak jedną kropką, więc tam kratka to znak; szerokość zdradzałaby, jakie znaki wpisano.
int Field::spanned(std::size_t from, std::size_t upto) const {
    int made = 0;

    std::size_t at = from;

    while (at < upto && at < text_.size()) {
        const std::size_t began = at;
        const char32_t one = utf8::decode(text_, at);

        made += concealed_ ? 1 : width(one);

        if (began == at) break;
    }

    return made;
}

Field::Field(int wide, std::string text) : text_(std::move(text)), wide_(wide) {
    role("talcite.input");

    at_ = text_.size();
}

int Field::capacity() const { return 0; }


Size Field::wants(Size room) const {
    return {std::min(std::max(wide_, 0), room.wide), std::min(1, room.high)};
}

// Pole jest jedynym widżetem, który ma co powiedzieć o kursorze — i mówi to **we własnych
// współrzędnych**, bez oglądania się na widoczny kawałek: od przycięcia jest korzeń. Odpowiedź
// jest prawdziwa dopiero po złożeniu klatki, bo `from_` — czyli o ile treść jest przewinięta —
// wylicza się w `paint`, gdy znana jest szerokość, którą pole naprawdę dostało.
std::optional<Spot> Field::caret() const {
    if (!focused()) return {};

    return Spot{spanned(from_, at_), 0};
}

std::size_t Field::cursor() const {
    std::size_t made = 0;
    std::size_t at = 0;

    while (at < at_ && at < text_.size()) {
        const std::size_t began = at;

        (void)utf8::decode(text_, at);

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
//
// Pole ukryte nie podpowiada wcale: podpowiedź pokazuje tekst jawnie, a jej źródło — historia,
// katalog — mogłoby zdradzić dawny sekret albo jego początek.
void Field::guessing() {
    ahead_.clear();

    if (!guess_ || concealed_ || at_ != text_.size()) return;

    const std::string whole = guess_(text_);

    if (whole.size() <= text_.size()) return;

    if (whole.compare(0, text_.size(), text_) != 0) return;

    ahead_ = whole.substr(text_.size());
}

// Co wolno wpisać. **Pole liczbowe to jedna lambda, a nie osobny widżet** — Midnight Commander nie
// ma widżetu liczbowego i liczby wpisuje w zwykłe `input`, więc nie ma czego naśladować. Ta sama
// furtka służy do zakazu ukośników w nazwie pliku czy do samych cyfr szesnastkowych.
void Field::allows(std::function<bool(char32_t one)> given) { allows_ = std::move(given); }

// **Pole ukryte** — na klucze i hasła: każdy znak rysuje się kropką, a podpowiedzi nie ma. Treść
// zostaje ta sama i `text()` oddaje ją jawnie; ukryty jest tylko ekran. Kopiowania pole i tak
// nie ma, więc nie ma czego wyłączać.
void Field::concealed(bool on) {
    concealed_ = on;

    guessing();
}

void Field::insert(const std::string& what) {
    std::string clean;

    clean.reserve(what.size());

    std::size_t at = 0;

    while (at < what.size()) {
        const std::size_t began = at;
        const char32_t one = utf8::decode(what, at);

        if (began == at) break;

        // Pole jest jednowierszowe, więc końce wierszy z wklejenia zamieniamy na odstępy zamiast
        // wpuszczać do napisu znak, którego nie umiemy narysować.
        const char32_t which = (one == U'\n' || one == U'\r') ? U' ' : one;

        // Odsiew działa **i przy pisaniu, i przy wklejaniu**: wklejone „12ab34" w polu na cyfry
        // zostawia „1234", zamiast odrzucać całość. Odrzucenie wszystkiego karałoby za jeden zły
        // znak w środku, a przepuszczenie kłamałoby o tym, co pole przyjmuje.
        if (allows_ && !allows_(which)) continue;

        utf8::encode(which, clean);
    }

    if (clean.empty()) return;

    text_.insert(at_, clean);
    at_ += clean.size();

    guessing();
}

View Field::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — ta sama reguła i ten sam powód co w przycisku.
    made.clear({{0, 0}, seen.size});

    const int room = std::max(given.wide, 0);

    // **Przewinięcie liczy się tutaj**, bo dopiero tu znana jest szerokość, którą pole naprawdę
    // dostało — rodzic ma prawo dać mniej, niż prosiło. To jedyne miejsce, gdzie malowanie zmienia
    // stan, i dzieje się tak świadomie: inaczej kursor potrafiłby wyjechać poza widoczny kawałek.
    if (at_ < from_) from_ = at_;

    while (room > 0 && spanned(from_, at_) >= room) from_ = after(text_, from_);

    const Skin& how = skin();

    // **Podkreślenie nie jest już tutaj** — siedzi we wbudowanej skórce, w roli `talcite.input`.
    // W Midnight Commanderze pole wyróżnia sam kolor tła, więc gdy dojdą barwy, wystarczy podmienić
    // wpis w skórce i z kodu nie ubędzie ani nie przybędzie ani jedna linijka.
    const Style look = how.style(enabled() ? role() : role() + ".disabled");

    made.fill({{-seen.at.x, -seen.at.y}, {room, 1}}, look);

    // Widoczny kawałek treści, przycięty do szerokości.
    std::size_t upto = from_;
    int wide = 0;

    while (upto < text_.size()) {
        const std::size_t began = upto;
        const std::size_t next = after(text_, upto);
        const int much = spanned(began, next);

        if (wide + much > room) break;

        wide += much;
        upto = next;

        if (began == upto) break;
    }

    if (concealed_) {
        const std::string dot = utf8::encode(how.glyph("talcite.input.conceal"));

        std::string dots;

        for (int which = 0; which < wide; ++which) dots += dot;

        made.put({-seen.at.x, -seen.at.y}, dots, look);
    } else {
        made.put({-seen.at.x, -seen.at.y}, text_.substr(from_, upto - from_), look);
    }

    // Podpowiedź **wyszarzona**, za treścią, na tyle, ile zostało miejsca. Nie jest częścią napisu
    // i nie wchodzi do `text()` — dopóki nikt jej nie przyjmie.
    if (!ahead_.empty() && wide < room) {
        const Style dim = Style::merge(look, how.style(role() + ".guess"));

        made.put({wide - seen.at.x, -seen.at.y}, ahead_, dim);
    }

    return made;
}

// **Enter pole ogłasza jako `SubmitEvent`** — „wpis zatwierdzony" — i nic poza tym nie robi: nie
// zna okna ani jego przycisków. Kto chce, żeby Enter w polu zatwierdzał formularz, wiesza uchwyt na
// formularzu (zdarzenie idzie w górę, więc jeden uchwyt obejmuje każde pole) i tam naciska swoje
// OK. Klawisz bierzemy tylko wtedy, gdy ktoś zdarzenie wziął; inaczej idzie dalej jak dawniej.
// Dawniej ten komentarz mówił, że samo oddanie Entera zatwierdza dialog — prawdą przestało to być,
// gdy okno przestało szukać przycisku podstawowego, i aplikacja musiała łatać to po swojemu.
//
// Esc i klawisze funkcyjne **oddajemy dalej** — to dzięki temu Esc w polu zamyka dialog, jak
// w Midnight Commanderze. Zwykłe litery bierzemy **zawsze**: bez tego „t" w polu naciskałoby
// przycisk, który ma tę literę w klawiszach aktywnych.
//
// Tab bierzemy **tylko wtedy, gdy jest co uzupełnić**. W innym razie oddajemy go dalej i ognisko
// przechodzi do następnego — tak samo rozstrzygają to powłoki.
bool Field::handle(KeyEvent& what) {
    if (!enabled()) return false;

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

        case Code::enter: return trigger(SubmitEvent{});

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

    utf8::encode(one, said);

    // Znak odsiany **połykamy**, a nie oddajemy wyżej. Klawisz był skierowany do pola i to, że pole
    // go nie chce, nie znaczy, że ma go dostać ktoś inny — inaczej „t" odrzucone w polu na cyfry
    // naciskałoby w dialogu przycisk „Tak".
    insert(said);

    return true;
}

bool Field::handle(PasteEvent& what) {
    if (!enabled()) return false;

    insert(what.text());

    return true;
}

// Klik stawia kursor tam, gdzie kliknięto, i bierze ognisko. Kolumnę przeliczamy tą samą miarą,
// którą maluje `paint` — inaczej kursor lądowałby obok miejsca, w które celowano.
bool Field::handle(ClickEvent& what) {
    if (!enabled()) return true;

    if (root() != nullptr) focus();

    const int wanted = local(what.where()).x;

    std::size_t at = from_;
    int wide = 0;

    while (at < text_.size()) {
        const std::size_t next = after(text_, at);
        const int much = spanned(at, next);

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
