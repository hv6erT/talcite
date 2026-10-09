#include "talcite/stack.hpp"

#include <algorithm>

namespace talcite::ui {

// **Jedyny pojemnik, w którym dzieci leżą na sobie.** `Row` i `Column` kładą obok; `Frame` owija;
// wszystko, co ma leżeć **na** czymś — zasłona na treści, rozwinięta lista na panelach — leży tu.
// Przedtem robiły to trzy ramy, każda po swojemu: pasek menu malował rozwinięte menu na końcu
// własnego `paint`, okno zasłon malowało zasłony na końcu własnego, a próby układu miały czwartą
// kopię z sortowaniem po numerze warstwy, którego biblioteka nie znała.
//
// **Warstwa to kolejność dzieci i nic więcej.** Ostatnie dopięte jest na wierzchu — tak maluje ten
// pojemnik i tak trafia mysz w `Root::under`, z tej samej listy. Numeru warstwy nie ma, bo byłby
// drugim źródłem tej samej prawdy; był, i był martwy.
//
// **Gdzie leży dziecko, mówi ono samo** przez `placement(room)`. Domyślnie cały pokój — tyle bierze
// zasłona (żeby łapać mysz wszędzie) i tyle bierze treść. Mniej bierze to, co ma stać w jednym
// miejscu na treści, i wtedy samo liczy, gdzie — zwykle z `standing()` tego, pod czym stoi. To jest
// całe pozycjonowanie w bibliotece: jedno pytanie, w jednym pojemniku, zamiast trybów na każdym
// węźle i pojemników, które musiałyby te tryby rozumieć.
int Stack::capacity() const { return plenty; }

Size Stack::wants(Size room) const { return room; }

View Stack::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`. Dziecko, które
    // bierze mniej niż cały pokój, nie zamalowuje reszty, więc spod niego widać niższe warstwy.
    made.clear({{0, 0}, seen.size});

    for (const std::shared_ptr<Node>& one : children()) {
        Area where = one->placement(given);

        // Dziecko może wejść na rodzeństwo, ale nie może wyjść poza rodzica — tej granicy pilnuje
        // pojemnik, nie każde dziecko z osobna, bo tu jest jedno miejsce, a tam byłoby ich tyle,
        // ile klas.
        where.at.x = std::clamp(where.at.x, 0, std::max(given.wide, 0));
        where.at.y = std::clamp(where.at.y, 0, std::max(given.high, 0));
        where.size.wide = std::clamp(where.size.wide, 0, given.wide - where.at.x);
        where.size.high = std::clamp(where.size.high, 0, given.high - where.at.y);

        const Area part = Area::crossed(seen, where);

        made.draw(one->render(where, part - where.at), part.at - seen.at);
    }

    return made;
}

}
