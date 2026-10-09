#include "talcite/pool.hpp"

#include <algorithm>
#include <utility>

namespace talcite {

namespace {

std::size_t roomy(std::size_t most) {
    if (most != 0) return most;

    const unsigned told = std::thread::hardware_concurrency();

    return told == 0 ? 1 : static_cast<std::size_t>(told);
}

// Dzwonek zadania, które biegnie właśnie na tym wątku. Blokująca operacja napisana w talcite
// czeka na swój deskryptor razem z tym dzwonkiem, więc drop() budzi ją normalnie, zamiast
// przerywać jej wywołanie systemowe. Poza zadaniem jest pusty.
thread_local Bell* standing = nullptr;

}

Bell* ringing() { return standing; }

Pool::Pool(std::size_t most) : most_(roomy(most)) {}

Pool::~Pool() {
    {
        const std::lock_guard<std::mutex> held{guard_};
        closing_ = true;
    }

    stirred_.notify_all();

    // **Wykaz zabieramy pod zamkiem, dołączamy poza nim.** `handed` ma prawo dopisać robotnika
    // z innego wątku — domknięcie zwalniane po zadaniu potrafi wrzucić ostatnią pracę — a wtedy
    // wektor się przealokowuje i pętla zakresowa czyta zwolnioną pamięć. Dołączać pod zamkiem
    // nie wolno, bo robotnik przy kończeniu sam po ten zamek sięga.
    std::vector<std::thread> these;

    {
        const std::lock_guard<std::mutex> held{guard_};
        these.swap(staff_);
    }

    for (std::thread& one : these)
        if (one.joinable()) one.join();
}

// **Jedno słowo na „oddaj do wykonania"**, to samo co `Loop::post` — raz na pętlę, raz do puli
// (przegląd, krok 6d). Tor mówi, czy praca czeka na system (`io`), czy liczy (`cpu`); dawniej były
// to dwie nazwy, take i easily, a druga nie mówiła, że chodzi o liczenie.
Pool::Job Pool::post(std::function<void()> what, Lane lane) {
    return handed(std::move(what), lane, nullptr);
}

// **Numer wpisany, zanim zadanie stanie się widoczne.** Wersja bez miejsca oddaje numer dopiero
// z `return`, czyli już po wstawieniu do kolejki i obudzeniu robotnika — a wtedy ten, kto chce
// zadanie odwołać, ma okno, w którym ono już biegnie, a numeru jeszcze nie ma. `drop()` nie ma
// wówczas czego zdjąć ani czyim dzwonkiem zadzwonić, więc anulowana praca przechodzi całą robotę
// do końca. Zmierzone na 4000 obietnic: na 1523 uruchomione ciała 467 nie usłyszało dzwonka.
Pool::Job Pool::post(std::function<void()> what, Lane lane, std::atomic<Job>& into) {
    return handed(std::move(what), lane, &into);
}

// Dwie kolejki, jeden zespół. Praca na torze `io` ma prawo czekać na plik albo na martwy udział
// sieciowy, więc pula rośnie dla niej bez ograniczeń — inaczej dziewiąte takie zadanie
// zatrzymałoby cały program. Praca na torze `cpu` deklaruje, że tylko liczy, więc nie ma sensu
// puszczać jej więcej naraz, niż maszyna ma rdzeni: dwustu liczących na ośmiu rdzeniach
// policzy wolniej niż ośmiu.
Pool::Job Pool::handed(std::function<void()> what, Lane lane, std::atomic<Job>* into) {
    std::unique_lock<std::mutex> held{guard_};

    const Job which = ++next_;

    // **Przed wstawieniem do kolejki, nie po.** Robotnik zdejmuje zadanie spod tego samego zamka,
    // więc kto zobaczy tu numer, ten ma pewność, że zadanie jest co najmniej w kolejce — a kto
    // zobaczy zero, ten wie, że żaden robotnik go jeszcze nie podniósł.
    if (into != nullptr) into->store(which);

    if (lane == Lane::cpu) cpu_.push_back(Doing{which, std::move(what)});
    else io_.push_back(Doing{which, std::move(what)});

    // Porównujemy z długością kolejki, a nie z zerem. Robotnik obudzony przed chwilą jest
    // wciąż liczony jako wolny, dopóki sam nie zdejmie zadania — więc reguła „stawiaj, gdy
    // nikt nie jest wolny" przy garści zadań wrzuconych naraz stawia jednego i każe reszcie
    // czekać w kolejce.
    const bool hasty = io_.size() > idle_;
    const bool slack = !hasty && cpu_.size() > idle_ && spare_ < most_;

    if (hasty || slack) {
        staff_.emplace_back([this] { serve(); });
        ++idle_;

        // Robotnicy postawieni dla pracy liczącej liczeni osobno: inaczej praca blokująca, która
        // zajęła cały zespół, zagłodziłaby liczącą — sufit byłby wyczerpany cudzymi wątkami.
        if (slack) ++spare_;
    }

    held.unlock();
    stirred_.notify_one();

    return which;
}

bool Pool::pulled(std::deque<Doing>& from, Job which) {
    const auto at = std::find_if(from.begin(), from.end(),
                                 [which](const Doing& one) { return one.which == which; });

    if (at == from.end()) return false;

    from.erase(at);

    return true;
}

// Zadanie, które jeszcze nie ruszyło, po prostu wypada z kolejki i nie kosztuje nic. Zadanie,
// które biegnie, dostaje dzwonek — nie przerywamy mu wywołania systemowego, tylko budzimy je
// z czekania, o ile samo czekało na dzwonek razem ze swoim deskryptorem.
void Pool::drop(Job which) {
    const std::lock_guard<std::mutex> held{guard_};

    if (pulled(io_, which) || pulled(cpu_, which)) {
        emptied_.notify_all();
        return;
    }

    const auto at = running_.find(which);
    if (at != running_.end()) at->second->ring();
}

void Pool::drain() {
    std::unique_lock<std::mutex> held{guard_};
    emptied_.wait(held, [this] { return io_.empty() && cpu_.empty() && busy_ == 0; });
}

std::size_t Pool::staff() const {
    const std::lock_guard<std::mutex> held{guard_};

    return staff_.size();
}

void Pool::serve() {
    Bell bell;

    standing = &bell;

    while (true) {
        Doing one;
        bool cpu = false;

        {
            std::unique_lock<std::mutex> held{guard_};

            stirred_.wait(held, [this] {
                return closing_ || !io_.empty() || (!cpu_.empty() && cpu_busy_ < most_);
            });

            if (!io_.empty()) {
                one = std::move(io_.front());
                io_.pop_front();
            } else if (!cpu_.empty() && (cpu_busy_ < most_ || closing_)) {
                one = std::move(cpu_.front());
                cpu_.pop_front();

                cpu = true;
                ++cpu_busy_;
            } else {
                if (closing_) {
                    standing = nullptr;
                    return;
                }

                continue;
            }

            // Dzwonek czyścimy pod tym samym zamkiem, pod którym zadanie wchodzi do biegnących.
            // Inaczej dzwonienie z drop() tuż przed końcem poprzedniego zadania zostawiłoby
            // dzwonek podniesiony, a następne zadanie obudziłoby się bez powodu.
            bell.clear();
            running_.emplace(one.which, &bell);

            --idle_;
            ++busy_;
        }

        // Wyjątek stąd nie ma dokąd pójść: obietnica zapisała go już u siebie jako odrzucenie,
        // a wątek roboczy nie ma komu go podać. Zjadamy go świadomie, nie przez przeoczenie.
        try {
            one.what();
        } catch (...) {
        }

        // **Domknięcie zwalniamy, zanim ogłosimy, że jesteśmy wolni.** Trzyma ono to, co złapał
        // wołający — obietnicę, jej wynik, cudze uchwyty — a `drain()` czeka na `busy_ == 0`
        // i po powrocie ma prawo sądzić, że nikt już tego nie dotyka. Zostawione do końca obrotu
        // pętli kasowałoby się **po** tym ogłoszeniu, na cudzym wątku i bez żadnej umowy z tym,
        // kto właśnie ruszył dalej.
        //
        // Zwalniamy **bez zamka**: kasowanie domknięcia uruchamia cudze destruktory, a te mają
        // prawo wrócić do puli.
        one.what = nullptr;

        {
            const std::lock_guard<std::mutex> held{guard_};

            running_.erase(one.which);

            --busy_;
            ++idle_;
            if (cpu) --cpu_busy_;
        }

        // Zwolnione miejsce liczącej pracy może przyjąć jeden czekający robotnik — budzimy jednego,
        // a nie wszystkich. Praca blokująca miejsca nie zajmuje, więc po niej nikogo budzić nie
        // trzeba: kolejkę sprawdzi ten robotnik sam, w następnym obrocie.
        emptied_.notify_all();

        if (cpu) stirred_.notify_one();
    }
}

Pool& pooling() {
    static Pool only;

    return only;
}

}
