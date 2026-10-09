#include "talcite/pixels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include "lighting.hpp"

namespace talcite {

namespace {

constexpr std::size_t bands = 4;

// Prostokąt w źródle, który przypada na jedną kratkę celu. Co najmniej jeden piksel, bo przy
// powiększaniu podział wychodziłby pusty — i wtedy zamiast rozmycia dostajemy najbliższego sąsiada,
// czyli ostre kwadraty. Dla ikony i pikselowej grafiki to jest **właściwa** odpowiedź, a nie
// zaniedbanie: rozmyty piksel-art wygląda gorzej niż powiększony.
[[nodiscard]] std::pair<int, int> spanned(int at, int from, int upto) {
    const int near = static_cast<int>(static_cast<std::int64_t>(at) * from / upto);
    const int far = static_cast<int>(static_cast<std::int64_t>(at + 1) * from / upto);

    return {near, std::max(near + 1, far)};
}

}

Pixels::Pixels(ui::Size given, std::vector<Still> stills)
    : size_{std::max(given.wide, 0), std::max(given.high, 0)}, stills_(std::move(stills)) {
    if (size_.wide == 0 || size_.high == 0) {
        if (!stills_.empty()) throw std::logic_error("talcite::Pixels: frames without dimensions");

        return;
    }

    const std::size_t much = static_cast<std::size_t>(size_.wide) *
                             static_cast<std::size_t>(size_.high) * bands;

    for (const Still& one : stills_)
        if (one.bytes.size() != much)
            throw std::logic_error("talcite::Pixels: the frame does not match the dimensions");
}

bool Pixels::held() const { return size_.wide > 0 && size_.high > 0 && !stills_.empty(); }

// Filtr pudełkowy: każda kratka celu jest średnią z prostokąta źródła, który na nią przypada.
// Przy zmniejszaniu to jest odpowiedź poprawna i najtańsza z poprawnych — bierze **wszystkie**
// piksele, więc cienka kreska nie znika przez to, że akurat nie trafiła w próbkę.
//
// Uśredniamy w świetle liniowym i **z alfą wmnożoną**. To drugie jest równie ważne co pierwsze:
// piksel przezroczysty niesie w pliku jakąś barwę, zwykle czarną, i wliczony na równi z widocznymi
// robi ciemną obwódkę wokół każdego wyciętego kształtu. Dlatego barwę ważymy alfą, a na końcu
// przez nią dzielimy — kratka bez ani jednego widocznego piksela zostaje po prostu pusta.
Pixels Pixels::scaled(ui::Size given) const {
    const ui::Size want{std::max(given.wide, 0), std::max(given.high, 0)};

    if (!held() || want.wide == 0 || want.high == 0) return {};

    if (want.wide == size_.wide && want.high == size_.high) return *this;

    const std::array<double, 256>& tone = straight();

    std::vector<Still> made;

    made.reserve(stills_.size());

    for (const Still& one : stills_) {
        Still fresh;

        fresh.after = one.after;
        fresh.bytes.resize(static_cast<std::size_t>(want.wide) *
                           static_cast<std::size_t>(want.high) * bands);

        for (int y = 0; y < want.high; ++y) {
            const auto [top, bottom] = spanned(y, size_.high, want.high);

            for (int x = 0; x < want.wide; ++x) {
                const auto [left, right] = spanned(x, size_.wide, want.wide);

                double sums[3] = {0.0, 0.0, 0.0};
                double alpha = 0.0;
                std::size_t seen = 0;

                for (int down = top; down < std::min(bottom, size_.high); ++down) {
                    const std::size_t row = static_cast<std::size_t>(down) *
                                            static_cast<std::size_t>(size_.wide) * bands;

                    for (int across = left; across < std::min(right, size_.wide); ++across) {
                        const std::size_t at = row + static_cast<std::size_t>(across) * bands;

                        const double weight =
                            static_cast<double>(sample(one.bytes[at + 3])) / 255.0;

                        for (std::size_t band = 0; band < 3; ++band)
                            sums[band] += tone[sample(one.bytes[at + band])] * weight;

                        alpha += weight;
                        ++seen;
                    }
                }

                const std::size_t into = (static_cast<std::size_t>(y) *
                                              static_cast<std::size_t>(want.wide) +
                                          static_cast<std::size_t>(x)) *
                                         bands;

                if (seen == 0 || alpha <= 0.0) {
                    fresh.bytes[into + 0] = std::byte{0};
                    fresh.bytes[into + 1] = std::byte{0};
                    fresh.bytes[into + 2] = std::byte{0};
                    fresh.bytes[into + 3] = std::byte{0};

                    continue;
                }

                for (std::size_t band = 0; band < 3; ++band)
                    fresh.bytes[into + band] = stored(bent(sums[band] / alpha));

                fresh.bytes[into + 3] = stored(static_cast<std::uint8_t>(
                    std::lround(alpha * 255.0 / static_cast<double>(seen))));
            }
        }

        made.push_back(std::move(fresh));
    }

    return Pixels{want, std::move(made)};
}

// Ile kratek zająć, żeby obrazek zachował proporcje. **Kratka nie jest kwadratem** — u jednych
// jest 10 na 20 pikseli, u innych 9 na 21 — więc bez jej wymiaru koło wychodzi jajkiem. Zero
// znaczy „terminal nie powiedział": przyjmujemy wtedy 1:2, bo tak wygląda przytłaczająca większość.
//
// Najpierw próbujemy zająć całą szerokość i liczymy wysokość; gdy ta nie mieści się w miejscu,
// zaczynamy od wysokości i liczymy szerokość. Zawsze co najmniej jedna kratka, bo obrazek zajmujący
// zero kratek to obrazek, którego nie widać, a wołający prosił o podgląd.
ui::Size fitting(ui::Size image, ui::Size room, ui::Size cell) {
    if (image.wide <= 0 || image.high <= 0 || room.wide <= 0 || room.high <= 0) return {};

    const std::int64_t wide = cell.wide > 0 ? cell.wide : 1;
    const std::int64_t high = cell.high > 0 ? cell.high : 2;

    const std::int64_t across = image.wide;
    const std::int64_t down = image.high;

    int columns = room.wide;
    int rows = static_cast<int>(std::max<std::int64_t>(
        (static_cast<std::int64_t>(columns) * wide * down + high * across / 2) / (high * across),
        1));

    if (rows > room.high) {
        rows = room.high;
        columns = static_cast<int>(std::clamp<std::int64_t>(
            (static_cast<std::int64_t>(rows) * high * across + wide * down / 2) / (wide * down), 1,
            room.wide));
    }

    return {columns, rows};
}

}
