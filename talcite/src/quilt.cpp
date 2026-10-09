#include "quilt.hpp"

#include <algorithm>
#include <utility>

namespace talcite {

namespace {

// Czy odcinek `after` zaczyna się w źródle dokładnie tam, gdzie kończy się `before` — wtedy dwa
// są jednym.
bool follows(const Quilt::Piece& before, const Quilt::Piece& after) {
    return before.added == after.added && before.at + before.much == after.at;
}

}

Quilt::Quilt(Bytes given) : held_(std::move(given)), whole_(held_.size()) {
    if (whole_ > 0) pieces_.push_back(Piece{false, 0, whole_});
}

// Numer odcinka, w którym leży bajt `at` (ostatniego zaczynającego się nie dalej niż `at`), po
// początkach — binarnie. Początki liczą się od nowa przy pierwszym pytaniu po zmianie.
std::size_t Quilt::located(std::size_t at) {
    if (stale_) {
        starts_.resize(pieces_.size());

        std::size_t gone = 0;

        for (std::size_t which = 0; which < pieces_.size(); ++which) {
            starts_[which] = gone;
            gone += pieces_[which].much;
        }

        stale_ = false;
    }

    const auto after = std::upper_bound(starts_.begin(), starts_.end(), at);

    return after == starts_.begin() ? 0 : static_cast<std::size_t>(after - starts_.begin()) - 1;
}

// Oddaje numer odcinka **zaczynającego się** dokładnie w `at`, rozcinając ten, w którego środek
// wypada. Dla końca dokumentu oddaje numer za ostatnim.
std::size_t Quilt::split(std::size_t at) {
    if (at >= whole_) return pieces_.size();

    const std::size_t which = located(at);
    const std::size_t gone = starts_[which];

    if (gone == at) return which;

    const Piece one = pieces_[which];
    const std::size_t took = at - gone;

    pieces_[which] = Piece{one.added, one.at, took};

    pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(which) + 1,
                   Piece{one.added, one.at + took, one.much - took});

    stale_ = true;

    return which + 1;
}

// Bajty spod `at`: odcinek początkowy binarnie, potem kolejne — każdy jednym kopiowaniem.
Bytes Quilt::bytes(std::size_t at, std::size_t most) {
    Bytes made;

    if (at >= whole_ || most == 0) return made;

    const std::size_t upto = std::min(at + most, whole_);

    made.reserve(upto - at);

    const std::size_t first = located(at);

    std::size_t gone = starts_[first];

    for (std::size_t which = first; which < pieces_.size() && gone < upto; ++which) {
        const Piece& one = pieces_[which];
        const std::size_t ends = gone + one.much;

        const std::size_t from = at > gone ? at - gone : 0;
        const std::size_t want = std::min(one.much, upto - gone) - from;

        if (one.added) {
            const auto* said = reinterpret_cast<const std::byte*>(added_.data()) + one.at + from;

            made.insert(made.end(), said, said + want);
        } else {
            const auto begin = held_.begin() + static_cast<std::ptrdiff_t>(one.at + from);

            made.insert(made.end(), begin, begin + static_cast<std::ptrdiff_t>(want));
        }

        gone = ends;
    }

    return made;
}

void Quilt::insert(std::size_t at, std::string_view what) { put(at, stored(what)); }

void Quilt::erase(std::size_t at, std::size_t much) {
    if (much == 0 || at >= whole_) return;

    const std::size_t took = std::min(much, whole_ - at);

    // Rozcinamy najpierw wcześniej, potem później: drugie cięcie dzieje się za pierwszym, więc
    // pierwszego numeru nie unieważnia.
    const std::size_t from = split(at);
    const std::size_t upto = split(at + took);

    pieces_.erase(pieces_.begin() + static_cast<std::ptrdiff_t>(from),
                  pieces_.begin() + static_cast<std::ptrdiff_t>(upto));

    whole_ -= took;

    stale_ = true;
}

// Odcinki pod zakresem, przycięte do jego brzegów. Dokumentu nie rusza — nie rozcina nawet
// odcinków, bo skrawek niesie własne kopie opisów, a nie numery.
Quilt::Scrap Quilt::taken(std::size_t at, std::size_t much) {
    Scrap made;

    if (at >= whole_ || much == 0) return made;

    const std::size_t upto = std::min(at + much, whole_);
    const std::size_t first = located(at);

    std::size_t gone = starts_[first];

    for (std::size_t which = first; which < pieces_.size() && gone < upto; ++which) {
        const Piece& one = pieces_[which];
        const std::size_t from = at > gone ? at - gone : 0;
        const std::size_t want = std::min(one.much, upto - gone) - from;

        made.pieces.push_back(Piece{one.added, one.at + from, want});

        gone += one.much;
    }

    made.much = upto - at;

    return made;
}

// Tekst trafia do dopisków, ale jeszcze nie do dokumentu — stanie tam, gdzie go wstawi `put`.
Quilt::Scrap Quilt::stored(std::string_view what) {
    Scrap made;

    if (what.empty()) return made;

    made.pieces.push_back(Piece{true, added_.size(), what.size()});
    made.much = what.size();

    added_.append(what);

    return made;
}

// **Wstawienie odcinków, bez bajtów.** Na obu brzegach odcinek zlewa się z sąsiadem, gdy w źródle
// leżą jeden za drugim: pisanie w ciągu przedłuża ostatni odcinek (bez tego akapit dałby pięćset
// odcinków zamiast jednego, a od ich liczby zależy koszt wszystkiego powyżej), a cofnięcie
// skasowania zszywa dokument z powrotem w te same odcinki, które były przed nim.
void Quilt::put(std::size_t at, const Scrap& what) {
    if (what.much == 0) return;

    // Cięcie w miejscu wstawienia liczy się po dokumencie **sprzed** wstawienia; długość i początki
    // zmieniają się dopiero potem.
    const std::size_t which = split(std::min(at, whole_));

    std::size_t first = 0;
    std::size_t last = what.pieces.size();

    if (which > 0 && follows(pieces_[which - 1], what.pieces.front())) {
        pieces_[which - 1].much += what.pieces.front().much;
        ++first;
    }

    if (first < last && which < pieces_.size() && follows(what.pieces[last - 1], pieces_[which])) {
        pieces_[which].at = what.pieces[last - 1].at;
        pieces_[which].much += what.pieces[last - 1].much;
        --last;
    }

    pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(which),
                   what.pieces.begin() + static_cast<std::ptrdiff_t>(first),
                   what.pieces.begin() + static_cast<std::ptrdiff_t>(last));

    // Wszystko weszło w lewego sąsiada — i ten może teraz stykać się z prawym.
    if (first == last && which > 0 && which < pieces_.size() && follows(pieces_[which - 1], pieces_[which])) {
        pieces_[which - 1].much += pieces_[which].much;
        pieces_.erase(pieces_.begin() + static_cast<std::ptrdiff_t>(which));
    }

    whole_ += what.much;

    stale_ = true;
}

// Końce wierszy w skrawku, liczone w źródłach, bez kopiowania.
std::size_t Quilt::lines(const Scrap& what) const {
    std::size_t made = 0;

    for (const Piece& one : what.pieces) {
        if (one.added) {
            const auto begin = added_.begin() + static_cast<std::ptrdiff_t>(one.at);

            made += static_cast<std::size_t>(std::count(begin, begin + static_cast<std::ptrdiff_t>(one.much), '\n'));
        } else {
            const auto begin = held_.begin() + static_cast<std::ptrdiff_t>(one.at);

            made += static_cast<std::size_t>(
                std::count(begin, begin + static_cast<std::ptrdiff_t>(one.much), std::byte{'\n'}));
        }
    }

    return made;
}

std::string Quilt::text(const Scrap& what) const {
    std::string made;

    made.reserve(what.much);

    for (const Piece& one : what.pieces) {
        if (one.added) {
            made.append(added_, one.at, one.much);
        } else {
            const auto* said = reinterpret_cast<const char*>(held_.data()) + one.at;

            made.append(said, one.much);
        }
    }

    return made;
}

// Ostatni bajt skrawka; zero dla pustego.
char Quilt::last(const Scrap& what) const {
    if (what.pieces.empty()) return '\0';

    const Piece& one = what.pieces.back();

    return one.added ? added_[one.at + one.much - 1] : static_cast<char>(held_[one.at + one.much - 1]);
}

// Dołożenie skrawka z przodu albo z tyłu, ze zlaniem odcinków na styku. Kasowanie wstecz (trzymany
// Backspace) dokłada z przodu po znaku, a każdy znak leży w źródle tuż przed poprzednim — więc
// odcinek się wydłuża i nic nie przesuwa. Przesunięcie wektora zdarza się tylko na granicy
// odcinków dokumentu, a nie przy każdym znaku.
void Quilt::joined(Scrap& into, const Scrap& more, bool front) {
    if (more.much == 0) return;

    std::vector<Piece>& to = into.pieces;
    const std::vector<Piece>& from = more.pieces;

    into.much += more.much;

    if (front) {
        std::size_t upto = from.size();

        if (!to.empty() && follows(from.back(), to.front())) {
            to.front().at = from.back().at;
            to.front().much += from.back().much;
            --upto;
        }

        to.insert(to.begin(), from.begin(), from.begin() + static_cast<std::ptrdiff_t>(upto));

        return;
    }

    std::size_t begin = 0;

    if (!to.empty() && follows(to.back(), from.front())) {
        to.back().much += from.front().much;
        ++begin;
    }

    to.insert(to.end(), from.begin() + static_cast<std::ptrdiff_t>(begin), from.end());
}

// Nowa lista odcinków w jednym przejściu: stare odcinki przed cięciem przechodzą (rozcięte na jego
// brzegach), te pod cięciem odpadają, w jego miejsce wchodzi odcinek dopisków. Koszt to liczba
// odcinków plus liczba cięć, a nie ich iloczyn — dawniej każde cięcie szło osobno przez `insert`
// i `erase`, każde po całej liście.
void Quilt::spliced(const std::vector<Cut>& cuts) {
    std::vector<Piece> made;
    made.reserve(pieces_.size() + 2 * cuts.size());

    std::size_t which = 0;
    std::size_t gone = 0;
    std::size_t taken = 0;

    // Kawałek bieżącego odcinka, który jeszcze nie przeszedł: od `taken` do jego końca.
    const auto copied = [&](std::size_t upto) {
        while (which < pieces_.size() && gone < upto) {
            const Piece& one = pieces_[which];
            const std::size_t ends = gone + one.much;
            const std::size_t stop = std::min(ends, upto);

            if (stop > gone + taken) made.push_back(Piece{one.added, one.at + taken, stop - gone - taken});

            if (stop == ends) {
                gone = ends;
                taken = 0;
                ++which;
            } else {
                taken = stop - gone;

                return;
            }
        }
    };

    const auto skipped = [&](std::size_t upto) {
        while (which < pieces_.size() && gone + taken < upto) {
            const std::size_t ends = gone + pieces_[which].much;

            if (ends <= upto) {
                gone = ends;
                taken = 0;
                ++which;
            } else {
                taken = upto - gone;

                return;
            }
        }
    };

    std::size_t last_at = 0;
    std::size_t last_much = 0;
    bool had = false;

    std::size_t grown = 0;
    std::size_t shrunk = 0;

    for (const Cut& one : cuts) {
        const std::size_t at = std::min(one.at, whole_);
        const std::size_t much = std::min(one.much, whole_ - at);

        copied(at);
        skipped(at + much);

        shrunk += much;

        if (one.text.empty()) continue;

        if (!had || one.text != std::string_view{added_}.substr(last_at, last_much)) {
            last_at = added_.size();
            last_much = one.text.size();
            added_.append(one.text);
            had = true;
        }

        made.push_back(Piece{true, last_at, one.text.size()});

        grown += one.text.size();
    }

    copied(whole_);

    pieces_ = std::move(made);
    whole_ = whole_ + grown - shrunk;
    stale_ = true;
}

}
