#include "talcite/sixel.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "lighting.hpp"

namespace talcite {

// **Sixel** (G1): obraz jako ciąg znaków, który terminal rysuje pikselami. Sześć wierszy pikseli to
// jeden pas; w pasie każda barwa idzie osobnym przebiegiem, a znak `?`..`~` niesie sześć bitów —
// które z sześciu pikseli kolumny mają tę barwę. Koder jest nasz, bez przejściówki: to sto
// kilkadziesiąt wierszy i żadna biblioteka nie zrobi tego lepiej, a każda dołożyłaby szew.
//
// Trzy decyzje, które warto znać:
//
// - **Przezroczystość przez `P2 = 1`.** Piksel przezroczysty nie należy do żadnej barwy, a terminal
//   zostawia pod nim to, co było — tło kratki, którą `Term` właśnie wymalował. Brzeg logo na tle okna
//   wychodzi przez to taki, jak przy znakach: z tłem spodu.
// - **Wysokość do pełnych pasów.** Terminal, który dostaje niepełny pas, rysuje go do sześciu wierszy
//   i potrafi przewinąć ekran, gdy obraz stoi na dole. Obcinamy więc wysokość do wielokrotności
//   sześciu (obraz niższy niż sześć zostaje, jak jest) i tracimy najwyżej pięć wierszy pikseli.
// - **Paleta przez cięcie po medianie**, ważone liczbą pikseli, do `colors` barw. Obraz o mniejszej
//   liczbie barw (logo, ikona) dostaje je dokładnie. Zdjęcie idzie przez kubełki 5×5×5 bitów: tnie
//   się kubełki, nie barwy, a najbliższą barwę palety liczy się raz na kubełek. Plakat 800×1200
//   koduje się tak w kilkadziesiąt milisekund zamiast prawie sekundy.
//
// Nagłówek rastra `"1;1;szerokość;wysokość` mówi o kwadratowych pikselach i o rozmiarze obrazu —
// terminal może od razu zarezerwować miejsce.
namespace {

constexpr std::uint8_t solid = 128;

struct Tone {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    friend constexpr auto operator<=>(Tone, Tone) = default;
};

struct Counted {
    Tone tone;
    std::size_t many = 0;
};

// Cięcie po medianie: pudełko o największej rozpiętości na którymkolwiek kanale dzieli się na dwie
// połowy po liczbie pikseli, aż pudełek jest tyle, ile barw wolno. Barwą pudełka jest średnia ważona.
std::vector<Tone> palette_of(std::vector<Counted> all, std::size_t most) {
    std::vector<Tone> made;

    if (all.empty()) return made;

    if (all.size() <= most) {
        for (const Counted& one : all) made.push_back(one.tone);

        return made;
    }

    struct Box {
        std::size_t from = 0;
        std::size_t upto = 0;
        int widest = 0;
        int channel = 0;
    };

    // Rozpiętość pudełka liczy się raz, gdy pudełko powstaje — przy stu tysiącach barw zdjęcia
    // liczenie jej od nowa w każdym kroku byłoby kwadratowe.
    const auto measured = [&all](std::size_t from, std::size_t upto) {
        Box made{from, upto, -1, 0};

        if (upto - from < 2) {
            made.widest = 0;

            return made;
        }

        for (int band = 0; band < 3; ++band) {
            int least = 255;
            int largest = 0;

            for (std::size_t at = from; at < upto; ++at) {
                const Tone& one = all[at].tone;
                const int value = band == 0 ? one.red : band == 1 ? one.green : one.blue;

                least = std::min(least, value);
                largest = std::max(largest, value);
            }

            if (largest - least > made.widest) {
                made.widest = largest - least;
                made.channel = band;
            }
        }

        return made;
    };

    std::vector<Box> boxes{measured(0, all.size())};

    while (boxes.size() < most) {
        std::size_t chosen = boxes.size();
        int widest = 0;

        for (std::size_t at = 0; at < boxes.size(); ++at)
            if (boxes[at].widest > widest) {
                widest = boxes[at].widest;
                chosen = at;
            }

        if (chosen == boxes.size()) break;

        const Box box = boxes[chosen];
        const int channel = box.channel;

        const auto key = [channel](const Counted& one) {
            return channel == 0 ? one.tone.red : channel == 1 ? one.tone.green : one.tone.blue;
        };

        std::sort(all.begin() + static_cast<std::ptrdiff_t>(box.from), all.begin() + static_cast<std::ptrdiff_t>(box.upto),
                  [&key](const Counted& one, const Counted& two) { return key(one) < key(two); });

        std::size_t total = 0;

        for (std::size_t at = box.from; at < box.upto; ++at) total += all[at].many;

        std::size_t half = box.from;
        std::size_t gathered = 0;

        while (half < box.upto - 1 && gathered + all[half].many <= total / 2) gathered += all[half++].many;

        if (half == box.from) ++half;

        boxes[chosen] = measured(box.from, half);
        boxes.push_back(measured(half, box.upto));
    }

    for (const Box& box : boxes) {
        double sums[3] = {};
        std::size_t many = 0;

        for (std::size_t at = box.from; at < box.upto; ++at) {
            sums[0] += static_cast<double>(all[at].tone.red) * static_cast<double>(all[at].many);
            sums[1] += static_cast<double>(all[at].tone.green) * static_cast<double>(all[at].many);
            sums[2] += static_cast<double>(all[at].tone.blue) * static_cast<double>(all[at].many);
            many += all[at].many;
        }

        const auto mean = [many](double sum) {
            return static_cast<std::uint8_t>(std::clamp(sum / static_cast<double>(many) + 0.5, 0.0, 255.0));
        };

        made.push_back(Tone{mean(sums[0]), mean(sums[1]), mean(sums[2])});
    }

    return made;
}

std::size_t nearest(Tone one, const std::vector<Tone>& palette) {
    std::size_t made = 0;
    int least = std::numeric_limits<int>::max();

    for (std::size_t at = 0; at < palette.size(); ++at) {
        const int red = one.red - palette[at].red;
        const int green = one.green - palette[at].green;
        const int blue = one.blue - palette[at].blue;
        const int here = 2 * red * red + 4 * green * green + 3 * blue * blue;

        if (here < least) {
            least = here;
            made = at;
        }
    }

    return made;
}

void number(std::size_t value, std::string& into) { into += std::to_string(value); }

// Jeden przebieg barwy w pasie: znak powtórzony `run` razy — od czterech powtórzeń taniej przez `!`.
void flushed(char sign, std::size_t run, std::string& into) {
    if (run == 0) return;

    if (run >= 4) {
        into += '!';
        number(run, into);
        into += sign;

        return;
    }

    into.append(run, sign);
}

}

std::string sixel(const Pixels& one, std::size_t still, std::size_t colors) {
    if (!one.held() || still >= one.stills().size()) return {};

    const ui::Size size = one.size();
    const Bytes& bytes = one.stills()[still].bytes;

    const auto wide = static_cast<std::size_t>(size.wide);
    const std::size_t full = static_cast<std::size_t>(size.high);
    const std::size_t high = full >= 6 ? full - full % 6 : full;

    if (wide == 0 || high == 0 || bytes.size() < wide * full * 4) return {};

    colors = std::clamp<std::size_t>(colors, 1, 256);

    const std::size_t count = wide * high;

    const auto seen = [&bytes](std::size_t at) { return sample(bytes[at * 4 + 3]) >= solid; };

    const auto tone_at = [&bytes](std::size_t at) {
        return Tone{sample(bytes[at * 4]), sample(bytes[at * 4 + 1]), sample(bytes[at * 4 + 2])};
    };

    const auto key_of = [](Tone tone) {
        return (static_cast<std::size_t>(tone.red >> 3) << 10) | (static_cast<std::size_t>(tone.green >> 3) << 5) |
               static_cast<std::size_t>(tone.blue >> 3);
    };

    // Indeks barwy dla każdego piksela; `none` to piksel przezroczysty.
    constexpr std::uint16_t none = std::numeric_limits<std::uint16_t>::max();

    std::vector<std::uint16_t> picked(count, none);
    std::vector<Tone> palette;

    // Najpierw próba dokładna: barwy liczone wprost, póki jest ich nie więcej niż wolno. Logo i ikona
    // kończą tu, z paletą po kolei barw i indeksem przez wyszukiwanie binarne.
    std::unordered_map<std::uint32_t, std::size_t> exact;
    bool fits = true;

    exact.reserve(colors * 2);

    for (std::size_t at = 0; at < count && fits; ++at) {
        if (!seen(at)) continue;

        const Tone tone = tone_at(at);

        ++exact[(static_cast<std::uint32_t>(tone.red) << 16) | (static_cast<std::uint32_t>(tone.green) << 8) | tone.blue];
        fits = exact.size() <= colors;
    }

    if (fits) {
        for (const auto& [key, many] : exact)
            palette.push_back(Tone{static_cast<std::uint8_t>(key >> 16), static_cast<std::uint8_t>(key >> 8),
                                   static_cast<std::uint8_t>(key)});

        std::sort(palette.begin(), palette.end());

        for (std::size_t at = 0; at < count; ++at)
            if (seen(at))
                picked[at] = static_cast<std::uint16_t>(std::lower_bound(palette.begin(), palette.end(), tone_at(at)) -
                                                        palette.begin());
    } else {
        // Zdjęcie: barwy idą do kubełków 5×5×5 bitów z sumami kanałów, więc cięcie po medianie dzieli
        // najwyżej 32768 kubełków, a nie setki tysięcy barw, a średnia pudełka i tak wychodzi dokładna.
        // Kubełek dostaje najbliższą barwę palety raz — dla swojej średniej.
        struct Bin {
            std::size_t many = 0;
            std::uint64_t sums[3] = {};
        };

        std::vector<Bin> bins(32 * 32 * 32);

        for (std::size_t at = 0; at < count; ++at) {
            if (!seen(at)) continue;

            const Tone tone = tone_at(at);
            Bin& bin = bins[key_of(tone)];

            ++bin.many;
            bin.sums[0] += tone.red;
            bin.sums[1] += tone.green;
            bin.sums[2] += tone.blue;
        }

        std::vector<Counted> all;
        std::vector<std::size_t> keys;

        for (std::size_t key = 0; key < bins.size(); ++key) {
            const Bin& bin = bins[key];

            if (bin.many == 0) continue;

            const auto mean = [&bin](std::uint64_t sum) {
                return static_cast<std::uint8_t>((sum + bin.many / 2) / bin.many);
            };

            all.push_back(Counted{Tone{mean(bin.sums[0]), mean(bin.sums[1]), mean(bin.sums[2])}, bin.many});
            keys.push_back(key);
        }

        std::vector<Tone> means;

        means.reserve(all.size());

        for (const Counted& one : all) means.push_back(one.tone);

        palette = palette_of(std::move(all), colors);

        std::vector<std::uint16_t> chosen(bins.size(), none);

        for (std::size_t at = 0; at < keys.size(); ++at)
            chosen[keys[at]] = static_cast<std::uint16_t>(nearest(means[at], palette));

        for (std::size_t at = 0; at < count; ++at)
            if (seen(at)) picked[at] = chosen[key_of(tone_at(at))];
    }

    std::string made = "\x1bP0;1;0q\"1;1;";

    number(wide, made);
    made += ';';
    number(high, made);

    for (std::size_t at = 0; at < palette.size(); ++at) {
        const auto percent = [](std::uint8_t value) { return static_cast<std::size_t>((value * 100 + 127) / 255); };

        made += '#';
        number(at, made);
        made += ";2;";
        number(percent(palette[at].red), made);
        made += ';';
        number(percent(palette[at].green), made);
        made += ';';
        number(percent(palette[at].blue), made);
    }

    // Pas przechodzi się raz: każdy piksel dokłada swój bit do kolumny swojej barwy, a barwa pamięta
    // pierwszą i ostatnią zajętą kolumnę. Przebieg barwy zaczyna się od pustych kolumn jednym `!`
    // i kończy na ostatniej zajętej — puste kolumny na końcu nic nie rysują, więc ich nie piszemy.
    std::vector<std::uint8_t> bits(palette.size() * wide);
    std::vector<std::size_t> first(palette.size(), wide);
    std::vector<std::size_t> last(palette.size(), 0);
    std::vector<std::uint16_t> used;

    used.reserve(palette.size());

    for (std::size_t top = 0; top < high; top += 6) {
        const std::size_t rows = std::min<std::size_t>(6, high - top);

        used.clear();

        for (std::size_t row = 0; row < rows; ++row)
            for (std::size_t x = 0; x < wide; ++x) {
                const std::uint16_t which = picked[(top + row) * wide + x];

                if (which == none) continue;

                if (first[which] == wide) used.push_back(which);

                bits[which * wide + x] |= static_cast<std::uint8_t>(1u << row);
                first[which] = std::min(first[which], x);
                last[which] = std::max(last[which], x + 1);
            }

        std::sort(used.begin(), used.end());

        bool begun = false;

        for (const std::uint16_t which : used) {
            std::uint8_t* const line = bits.data() + static_cast<std::size_t>(which) * wide;

            if (begun) made += '$';

            begun = true;

            made += '#';
            number(which, made);

            flushed('?', first[which], made);

            char sign = 0;
            std::size_t run = 0;

            for (std::size_t x = first[which]; x < last[which]; ++x) {
                const char here = static_cast<char>(63 + line[x]);

                line[x] = 0;

                if (here == sign) {
                    ++run;

                    continue;
                }

                flushed(sign, run, made);

                sign = here;
                run = 1;
            }

            flushed(sign, run, made);

            first[which] = wide;
            last[which] = 0;
        }

        if (top + 6 < high) made += '-';
    }

    made += "\x1b\\";

    return made;
}

}
