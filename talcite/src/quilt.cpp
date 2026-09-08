#include "quilt.hpp"

#include <algorithm>
#include <utility>

namespace talcite {

Quilt::Quilt(Feed feed, std::size_t whole) : feed_(std::move(feed)), whole_(whole) {
    if (whole_ > 0) pieces_.push_back(Piece{false, 0, whole_});
}

Quilt::Quilt(Bytes given) : held_(std::move(given)), whole_(held_.size()) {
    if (whole_ > 0) pieces_.push_back(Piece{false, 0, whole_});
}

// Oddaje numer odcinka **zaczynającego się** dokładnie w `at`, rozcinając ten, w którego środek
// wypada. Dla końca dokumentu oddaje numer za ostatnim.
std::size_t Quilt::split(std::size_t at) {
    std::size_t gone = 0;

    for (std::size_t which = 0; which < pieces_.size(); ++which) {
        if (gone == at) return which;

        const Piece one = pieces_[which];

        if (at < gone + one.much) {
            const std::size_t took = at - gone;

            pieces_[which] = Piece{one.added, one.at, took};

            pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(which) + 1,
                           Piece{one.added, one.at + took, one.much - took});

            return which + 1;
        }

        gone += one.much;
    }

    return pieces_.size();
}

Bytes Quilt::bytes(std::size_t at, std::size_t most) {
    Bytes made;

    if (at >= whole_ || most == 0) return made;

    const std::size_t upto = std::min(at + most, whole_);

    made.reserve(upto - at);

    std::size_t gone = 0;

    for (const Piece& one : pieces_) {
        const std::size_t ends = gone + one.much;

        if (ends <= at) {
            gone = ends;

            continue;
        }

        if (gone >= upto) break;

        const std::size_t from = at > gone ? at - gone : 0;
        const std::size_t want = std::min(one.much, upto - gone) - from;

        if (one.added) {
            const char* said = added_.data() + one.at + from;

            for (std::size_t which = 0; which < want; ++which)
                made.push_back(static_cast<std::byte>(said[which]));
        } else if (!held_.empty()) {
            const auto begin = held_.begin() + static_cast<std::ptrdiff_t>(one.at + from);

            made.insert(made.end(), begin, begin + static_cast<std::ptrdiff_t>(want));
        } else if (feed_) {
            // Źródło ma prawo oddać mniej, niż poproszono, więc dobieramy w pętli — ta sama
            // reguła, co w szpuli.
            std::size_t took = 0;

            while (took < want) {
                const Bytes more = feed_(one.at + from + took, want - took);

                if (more.empty()) break;

                const std::size_t part = std::min(more.size(), want - took);

                made.insert(made.end(), more.begin(),
                            more.begin() + static_cast<std::ptrdiff_t>(part));

                took += part;
            }
        }

        gone = ends;
    }

    return made;
}

// Po zszyciu dokument jest jednym odcinkiem oryginału, a oryginałem jest to, co właśnie zebraliśmy.
// Dopiski idą do kosza razem ze śmieciem po skasowanym tekście — to jedyne miejsce, w którym one
// maleją.
//
// Gdyby źródło oddało mniej, niż obiecało, zszycie **nie dochodzi do skutku**: kołdra zostaje taka,
// jaka była, bo skrócenie dokumentu za plecami szpuli byłoby gorsze niż niezrobienie niczego.
bool Quilt::settle() {
    if (!feed_) return true;

    Bytes all = bytes(0, whole_);

    if (all.size() != whole_) return false;

    held_ = std::move(all);
    feed_ = {};

    added_.clear();
    added_.shrink_to_fit();

    pieces_.clear();

    if (whole_ > 0) pieces_.push_back(Piece{false, 0, whole_});

    return true;
}

void Quilt::insert(std::size_t at, std::string_view what) {
    if (what.empty()) return;

    const std::size_t where = std::min(at, whole_);
    const std::size_t from = added_.size();

    added_.append(what);

    whole_ += what.size();

    // **Pisanie w ciągu przedłuża ostatni odcinek**, zamiast robić nowy. Bez tego wpisanie akapitu
    // dałoby pięćset odcinków zamiast jednego, a od ich liczby zależy koszt wszystkiego poniżej.
    if (!pieces_.empty()) {
        const std::size_t which = split(where);

        if (which > 0) {
            Piece& kept = pieces_[which - 1];

            if (kept.added && kept.at + kept.much == from) {
                kept.much += what.size();

                return;
            }
        }

        pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(which),
                       Piece{true, from, what.size()});

        return;
    }

    pieces_.push_back(Piece{true, from, what.size()});
}

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
}

}
