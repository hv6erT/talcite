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
        throw std::logic_error("talcite::Loop: na tym wątku pętla już stoi");

    standing = this;
}

Loop::~Loop() {
    if (standing == this) standing = nullptr;
}

Loop* Loop::current() { return standing; }

void Loop::post(std::function<void()> what) {
    {
        const std::lock_guard<std::mutex> held{guard_};
        queued_.push_back(std::move(what));
    }

    bell_.ring();
}

std::size_t Loop::waiting() const {
    const std::lock_guard<std::mutex> held{guard_};

    return queued_.size();
}

// Kolejkę zabieramy w całości i oddajemy zamek przed wykonaniem. Inaczej wywołanie, które samo
// czegoś dopisze, zakleszczyłoby się na własnej pętli — a to najzwyklejszy przypadek, jaki jest.
//
// **Dzwonek opróżniamy przed zabraniem kolejki, nie po.** Odwrotna kolejność gubiła przebudzenie:
// `post` dopisuje pod zamkiem i dopiero potem dzwoni, więc wywołanie wciśnięte między zabranie
// a opróżnienie zostawiało pracę w kolejce, a jego dzwonek szedł do kosza — i pętla stawała na
// `poll` bez terminu, aż do najbliższego klawisza. Przy tej kolejności najgorsze, co się zdarzy,
// to jeden jałowy obrót.
std::size_t Loop::turn() {
    std::vector<std::function<void()>> taken;

    bell_.clear();

    {
        const std::lock_guard<std::mutex> held{guard_};
        taken.swap(queued_);
    }

    for (const std::function<void()>& one : taken) one();

    return taken.size();
}

}
