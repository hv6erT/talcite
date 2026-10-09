#include "talcite/detail/result.hpp"

#include <deque>
#include <functional>
#include <iterator>
#include <vector>

#include "talcite/promise.hpp"

namespace talcite::detail {

// **Wnętrzności obietnicy mieszkają w `talcite::detail`** (przegląd, krok 6e): bilet, wynik,
// ogniwa i złączenia. Publiczny `promise.hpp` niesie już tylko to, czego używa wołający —
// `Promise`, `Reject`, `Rejected` i `Cancelled` — a biblioteka, która składa własne bilety
// (postęp kopiowania, zgłoszenia sieciowe), sięga do `detail` jawnie.

// Odrzucenie czymś, co nie jest `std::exception`, trafia do `fail` jako ten jeden stały
// `Rejected`. Stoi tu, a nie w nagłówku, bo ogniwa w `detail` powstają przed definicją `Rejected`
// — a tak nie muszą jej znać.
const std::exception& unknown() {
    static const Rejected made{"Unknown reason for rejection."};

    return made;
}


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

namespace {

// **Trampolina budzenia.** Ciąg dalszy, który coś rozstrzyga, budzi następnych — dawniej wprost, na
// tym samym stosie, więc rozstrzygnięcie długiego łańcucha schodziło o piętro na ogniwo i przy stosie
// 1 MB (Windows) padało przy kilku tysiącach ogniw. Dziś budzi tylko najbardziej zewnętrzne `woken`
// na wątku; zagnieżdżone odkłada swoich obserwatorów do `batch`, a pętla po powrocie z `woke()` stawia
// ich na czele kolejki, w kolejności odkładania. Porządek zostaje ten sam co dawniej — w głąb,
// rodzeństwo po potomkach — zmienia się tylko chwila: potomek biegnie po powrocie ze swojego rodzica,
// a nie w jego trakcie.
//
// Kolejka leży na stosie zewnętrznego wywołania, a wątek zna ją tylko przez wskaźnik: zmienna wątku
// z niebanalnym destruktorem ginęłaby przy końcu wątku przed innymi, które jeszcze budzą.
struct Wakes {
    std::deque<std::shared_ptr<Waiter>> pending;
    std::vector<std::shared_ptr<Waiter>> batch;
};

thread_local Wakes* waking = nullptr;

// Wyjątek z `woke()` zostawiłby wskaźnik na martwą kolejkę; zdejmuje go destruktor, jakkolwiek pętla
// się skończy.
struct Draining {
    Draining(const Draining&) = delete;
    Draining& operator=(const Draining&) = delete;
    Draining(Draining&&) = delete;
    Draining& operator=(Draining&&) = delete;

    explicit Draining(Wakes& mine) { waking = &mine; }

    ~Draining() { waking = nullptr; }
};

// **Trampolina zwalniania** — to samo dla rozbiórki. Ogniwo trzyma rodzica (`upstream_`), więc
// zwolnienie ogona łańcucha zwalniało rodzica z wnętrza destruktora dziecka, piętro na ogniwo. Dziś
// zwalnia tylko najbardziej zewnętrzne `buried`; zagnieżdżone odkłada to, co trzymało, na stos
// zewnętrznego.
thread_local std::vector<Upward>* burying = nullptr;

struct Burying {
    Burying(const Burying&) = delete;
    Burying& operator=(const Burying&) = delete;
    Burying(Burying&&) = delete;
    Burying& operator=(Burying&&) = delete;

    explicit Burying(std::vector<Upward>& mine) { burying = &mine; }

    ~Burying() { burying = nullptr; }
};

// **Trampolina wspinaczki.** `start` i `cancel` idą w górę łańcucha, od ogona do źródła, i dawniej
// szły rekurencyjnie, piętro na ogniwo — trzecia droga, na której długi łańcuch wyczerpywał stos.
// Krok zagnieżdżony trafia do kolejki zewnętrznego; wołający widzi to samo co dawniej, bo zanim
// zewnętrzne wywołanie wróci, wszystkie kroki są zrobione. Kolejność kroków nie ma tu znaczenia, bo
// nie da się jej zobaczyć: `start` źródła tylko wkłada pracę do puli, a pula bierze ją wieloma wątkami
// — kolejka idzie więc po prostu od przodu.
using Climbs = std::deque<std::function<void()>>;

thread_local Climbs* climbing = nullptr;

struct Climbing {
    Climbing(const Climbing&) = delete;
    Climbing& operator=(const Climbing&) = delete;
    Climbing(Climbing&&) = delete;
    Climbing& operator=(Climbing&&) = delete;

    explicit Climbing(Climbs& mine) { climbing = &mine; }

    ~Climbing() { climbing = nullptr; }
};

}

void climbed(const std::function<void()>& step) {
    if (climbing != nullptr) {
        climbing->push_back(step);

        return;
    }

    Climbs mine;
    mine.push_back(step);

    const Climbing held{mine};

    while (!mine.empty()) {
        const std::function<void()> next = std::move(mine.front());
        mine.pop_front();

        next();
    }
}

void buried(Upward gone) {
    if (!gone.start && !gone.cancel) return;

    if (burying != nullptr) {
        burying->push_back(std::move(gone));

        return;
    }

    std::vector<Upward> graves;

    const Burying held{graves};

    { const Upward first = std::move(gone); }

    while (!graves.empty()) {
        const Upward next = std::move(graves.back());
        graves.pop_back();
    }
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
    if (waking != nullptr) {
        waking->batch.insert(waking->batch.end(), std::make_move_iterator(live.begin()),
                             std::make_move_iterator(live.end()));

        return;
    }

    Wakes mine;
    mine.pending.assign(std::make_move_iterator(live.begin()), std::make_move_iterator(live.end()));

    const Draining held{mine};

    while (!mine.pending.empty()) {
        const std::shared_ptr<Waiter> one = std::move(mine.pending.front());
        mine.pending.pop_front();

        one->woke();

        if (mine.batch.empty()) continue;

        mine.pending.insert(mine.pending.begin(), std::make_move_iterator(mine.batch.begin()),
                            std::make_move_iterator(mine.batch.end()));
        mine.batch.clear();
    }
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
