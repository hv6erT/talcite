#include "talcite/loop.hpp"

#include <stdexcept>
#include <utility>

namespace talcite {

namespace {

// Pętla należy do wątku, a nie do procesu. Dzięki temu drugi ekran albo wątek z własnym
// obiegiem jest możliwy, a testy nie dzielą stanu między sobą.
thread_local Loop* standing = nullptr;

}

Loop::Loop() {
    if (standing != nullptr)
        throw std::logic_error("talcite::Loop: a loop already stands on this thread");

    standing = this;
}

Loop::~Loop() {
    if (standing == this) standing = nullptr;
}

Loop* Loop::current() { return standing; }

// **Kolejka i dzwonek mieszkają w drzwiach (`detail::Door`), nie w samej pętli.** Pętla trzyma
// je mocno, ciąg dalszy obietnicy (`here`, `on`) — słabo. Dawniej ogniwo trzymało gołe wskazanie
// na pętlę, a praca puli, która kończyła się po jej zniszczeniu, wołała `post` na martwym obiekcie;
// gdy w tym samym miejscu pamięci stanęła nowa pętla, ciąg dalszy wykonywał się na cudzej. Dziś
// drzwi giną razem z pętlą, a wszystko, co po nich przyjdzie, przepada — i nie ma gdzie trafić.
void detail::Door::post(std::function<void()> what) {
    {
        const std::lock_guard<std::mutex> held{guard_};
        queued_.push_back(std::move(what));
    }

    bell_.ring();
}

std::vector<std::function<void()>> detail::Door::taken() {
    std::vector<std::function<void()>> made;

    const std::lock_guard<std::mutex> held{guard_};

    made.swap(queued_);

    return made;
}

std::size_t detail::Door::waiting() const {
    const std::lock_guard<std::mutex> held{guard_};

    return queued_.size();
}

void Loop::post(std::function<void()> what) { door_->post(std::move(what)); }

std::size_t Loop::waiting() const { return door_->waiting(); }

// Kolejkę zabieramy w całości i oddajemy zamek przed wykonaniem. Inaczej wywołanie, które samo
// czegoś dopisze, zakleszczyłoby się na własnej pętli — a to najzwyklejszy przypadek, jaki jest.
//
// **Dzwonek opróżniamy przed zabraniem kolejki, nie po.** Odwrotna kolejność gubiła przebudzenie:
// `post` dopisuje pod zamkiem i dopiero potem dzwoni, więc wywołanie wciśnięte między zabranie
// a opróżnienie zostawiało pracę w kolejce, a jego dzwonek szedł do kosza — i pętla stawała na
// `poll` bez terminu, aż do najbliższego klawisza. Przy tej kolejności najgorsze, co się zdarzy,
// to jeden jałowy obrót.
std::size_t Loop::turn() {
    door_->bell().clear();

    const std::vector<std::function<void()>> taken = door_->taken();

    for (const std::function<void()>& one : taken) one();

    return taken.size();
}

}
