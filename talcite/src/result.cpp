#include "talcite/result.hpp"

namespace talcite {

// Zwykła obietnica nie ma czego mierzyć: albo jeszcze trwa, albo już nie. Miarę wnoszą
// dopiero pochodne — te, które wiedzą, ile bajtów z ilu przepisały albo ile pozycji z ilu
// zeszły. Dlatego jest wirtualna i dlatego siedzi w bilecie, a nie w uchwycie: przycięcie
// obietnicy do klasy bazowej nie może zgubić sposobu liczenia.
double Ticket::progress() const { return over(*this) ? 1.0 : 0.0; }

// Porzucenie biletu, o którym wie tylko ten, kto pod nim naprawdę coś trzyma. Zwykłej obietnicy
// nie ma z czego zdejmować: anulowanie zabiera jej robotę z puli i to wszystko. Ale bilet sieciowy
// **jest jednocześnie zgłoszeniem leżącym w centrali** — samo odrzucenie zostawiłoby je tam na
// zawsze, z zajętym kierunkiem gniazda i z uchwytem trzymanym przez `kept`. Wirtualna z tego
// samego powodu co `progress()`: obietnica trzyma bilet przycięty do bazy i o pochodnej nie wie.
void Ticket::dropped() {}

bool over(const Ticket& ticket) {
    const State now = ticket.state.load();

    return now == State::resolved || now == State::rejected;
}

// Obserwatorzy trzymani są słabo, więc ten, kto zdążył zginąć, po prostu wypada. To jest całe
// odcięcie wywołań przy śmierci właściciela: nie ma osobnego mechanizmu, jest brak wskazania.
void woken(Ticket& ticket) {
    std::vector<std::shared_ptr<Waiter>> live;

    {
        const std::lock_guard<std::mutex> held{ticket.guard};

        live.reserve(ticket.watching.size());
        for (const std::weak_ptr<Waiter>& one : ticket.watching)
            if (std::shared_ptr<Waiter> kept = one.lock()) live.push_back(std::move(kept));

        ticket.watching.clear();
    }

    ticket.stirred.notify_all();

    // Poza zamkiem: wywołania użytkownika potrafią trwać i potrafią sięgnąć z powrotem tutaj.
    for (const std::shared_ptr<Waiter>& one : live) one->woke();
}

void spoiled(Ticket& ticket, std::exception_ptr why) {
    {
        const std::lock_guard<std::mutex> held{ticket.guard};
        if (over(ticket)) return;

        ticket.why = why;
        ticket.state.store(State::rejected);
    }

    woken(ticket);
}

// Rejestracja na rzeczy już rozstrzygniętej nie może przepaść — inaczej then dopięty o ułamek
// sekundy za późno milczałby na zawsze. Ale nie wolno jej też wykonać na miejscu: wywołanie
// zwrotne raz biegłoby przed powrotem z then, a raz po nim, zależnie od tego, czy praca
// zdążyła się skończyć. Dlatego zwracamy prawdę i zostawiamy odpalenie wołającemu, który wie,
// gdzie je odłożyć.
bool hooked(Ticket& ticket, const std::shared_ptr<Waiter>& one) {
    const std::lock_guard<std::mutex> held{ticket.guard};

    if (over(ticket)) return true;

    ticket.watching.push_back(one);

    return false;
}

}
