#include "talcite/buttons.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include "talcite/button.hpp"
#include "talcite/row.hpp"

namespace talcite::ui {

// **Rząd odpowiedzi okna.** Formularz i komunikat mają go tak samo: wiersz przycisków pośrodku,
// pod treścią. Wcześniej każde z nich trzymało własne `add` i `clear` obok `title` i `text`, więc
// „dodaj" na oknie nie mówiło, co dodaje. Teraz mówi to podobiekt: `buttons().add("OK")`.
//
// Podobiekt nie jest węzłem. Wiersz należy do okna i stoi w jego drzewie od początku; `Buttons`
// tylko go obsługuje, więc nikt poza oknem go nie zrobi — konstruktor jest prywatny.
//
// **Przycisk robi podobiekt, a oddaje go wołającemu.** Gdyby przyjmował gotowy przycisk, musiałby
// mu nadpisać rolę i wcisnąć się przed uchwyty, które wołający już na nim powiesił. Tak jest jedna
// kolejność i nie ma jej jak popsuć, a wołający dostaje pełny przycisk: `primary()`, `keys()`,
// `role()` i własne `on<PickEvent>`, w którym mieszka czynność tej odpowiedzi.
//
// Okno **nie wiesza na przycisku niczego swojego** i samo się po naciśnięciu nie zamyka.
// Zamknięcie jest częścią czynności odpowiedzi, więc należy do wołającego: jego uchwyt woła
// `close()`, kiedy uzna, że okno ma zejść. Nie każda odpowiedź kończy okno — przycisk „Pomoc"
// w formularzu nie ma go zamykać — a biblioteka nie zna się na tym lepiej niż aplikacja.
std::shared_ptr<Button> Buttons::add(std::string text) {
    auto one = std::make_shared<Button>(std::move(text));

    if (!role_.empty()) one->role(role_);

    row_->children().add(one);

    all_.push_back(one);

    return one;
}

// Sięgnięcie poza koniec jest błędem wołającego, a nie stanem do przemilczenia — tak samo jak
// w dzieciach węzła i w zbiorze opcji.
const std::shared_ptr<Button>& Buttons::operator[](std::size_t which) const {
    if (which >= all_.size()) throw std::out_of_range("talcite::ui::Buttons: no such button");

    return all_[which];
}

// Czyszczenie zabiera przyciski razem z odstępami — cały wiersz jest ich i niczyj więcej.
void Buttons::clear() {
    row_->children().clear();

    all_.clear();
}

// Rolę rozdaje komunikat, bo ma rodzinę ról (`<rdzeń>.button`); formularz własnych ról nie ma
// i nie woła tego wcale, więc jego przyciski rysują się jako `talcite.button`. Rola trafia do
// przycisków już stojących i do każdego następnego.
void Buttons::role(std::string given) {
    role_ = std::move(given);

    for (const std::shared_ptr<Button>& one : all_) one->role(role_);
}

}
