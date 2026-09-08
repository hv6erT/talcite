#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/feed.hpp"

namespace talcite {

// **Kołdra zszyta z kawałków: dokument, który da się zmieniać, nie mając go w pamięci.**
//
// Dokument to łańcuch odcinków, z których każdy mówi „tyle bajtów z oryginału od miejsca X" albo
// „tyle bajtów z dopisków od miejsca Y". Wpisanie znaku rozcina jeden odcinek na trzy, skasowanie
// rozcina i wyrzuca. **Oryginału nikt nigdy nie rusza i nikt go nie wczytuje** — zostaje za
// `Feed`em. Pamięć rośnie z liczbą zmian, nie z rozmiarem pliku.
//
// Tak robi Word i tak robi VS Code, i to nie przypadek: to jedyna struktura, która daje jedno
// i drugie naraz.
//
// Cała rzecz jest przy tym **zwykłym źródłem bajtów** — oddaje bajty spod podanego przesunięcia,
// czyli dokładnie to, czego chce `Spool`. Dzięki temu wiersze, zawijanie, znaczniki i pasek nie
// wiedzą, że dokument da się zmieniać.
class Quilt {
public:
    Quilt(Feed feed, std::size_t whole);

    explicit Quilt(Bytes given);

    [[nodiscard]] std::size_t whole() const { return whole_; }

    [[nodiscard]] Bytes bytes(std::size_t at, std::size_t most);

    // Wciąga cały dokument do pamięci i **odcina się od źródła**. Po tym plik, z którego czytało
    // źródło, wolno nadpisać: nic już do niego nie sięga. Kosztuje pamięć równą dokumentowi, więc
    // robi to ten, kto wie, że chce ją zapłacić.
    bool settle();

    void insert(std::size_t at, std::string_view what);

    void erase(std::size_t at, std::size_t much);

private:
    struct Piece {
        bool added = false;
        std::size_t at = 0;
        std::size_t much = 0;
    };

    [[nodiscard]] std::size_t split(std::size_t at);

    Feed feed_;
    Bytes held_;

    // **Dopiski tylko rosną i nigdy się nie kurczą.** Skasowany tekst zostaje w nich jako śmieć
    // i to nie jest przeoczenie: gdyby się kurczyły, każdy odcinek wskazujący dalej trzeba by
    // poprawiać, a wtedy koszt kasowania zależałby od liczby zmian, a nie od jednej.
    std::string added_;

    std::vector<Piece> pieces_;
    std::size_t whole_ = 0;
};

}
