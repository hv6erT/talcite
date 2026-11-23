#include "rasterizing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "tal/trouble.hpp"

// Rasterizing (E7a): the coverage of a polygon on every pixel, from the exact area each edge leaves
// on the cells it crosses (the accumulation of font-rs and of FreeType's gray rasterizer). An edge
// adds to each cell of its rows the signed area between it and the cell's right side, and the
// next cell the rest of the height it spans there; a running sum along the row then gives every
// cell its winding, weighted by how much of it lies inside. The nonzero rule takes the size of
// the winding up to one; the even-odd rule folds it by twos, so that one and three are inside and
// two is out.
//
// The sum is exact only where a cell holds no more than one edge: a cell half of winding two and
// half of winding zero sums to one, and a cell of +1 and -1 to nothing. Strokes overlap at every
// join, and self-crossing paths cross, so each row of pixels is cut into sixteen rows of cells and
// the pixel takes their mean: an edge crossing a cut row within one or two cells (all but the
// flattest) is a step there, at the middle of its part, and each such step is also noted on its
// cell. A cell of a cut row with two steps or more takes the rule along its width, from the
// winding at its left side through each step in order, rather than the rule of its sum, so that
// edges overlapping within a cell (the inner side of every turn of a stroke, where its pieces
// overlap) cover what their union covers. Flatter edges cross a cut row through many cells, each
// taking a sliver of area; those stay in the sum, where their error is at most the sliver's. So
// do edges left of the box, standing on its left side (a step at the very left is the winding
// the cell starts with), and a cell of more than `most_steps` steps in one cut row, which only a
// pile of overlapping edges brings. Cut in sixteen, the error left falls from up to 0.85 of a
// pixel to a few hundredths. The rows are cut
// band by band of 32 pixels, so that a large raster's cells never stand in memory all at once.
//
// The box is the part of the raster the polygon can reach. An edge is cut to its band; its parts
// left of the box stand on the box's left side (they still carry their winding into every pixel to
// their right), and its parts right of it are dropped, since they reach no pixel (an edge wholly
// right of the box is not even kept). Two cells beyond the right side of each row take what the
// last pixels pass on. The geometry is in
// double (long double for cutting), the coverage in float.
namespace tal::drawing::rasterizing {

namespace {

constexpr int band = 32;
constexpr std::size_t most_steps = 64;
constexpr double most_crossings = 1 << 25;

}

// The part of this box within `other`.
Box Box::common(const Box& other) const {
    const Box made{std::max(x0, other.x0), std::max(y0, other.y0), std::min(x1, other.x1), std::min(y1, other.y1)};

    return made.empty() ? Box{made.x0, made.y0, made.x0, made.y0} : made;
}

// The pixels the points of `lines` can reach, within `within`.
Box bounded(const std::vector<flattening::Line>& lines, const Box& within) {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    bool any = false;

    for (const flattening::Line& line : lines) {
        for (const Point& one : line.points) {
            if (!any) {
                left = right = one.x;
                top = bottom = one.y;
                any = true;
            }

            left = std::min(left, one.x);
            right = std::max(right, one.x);
            top = std::min(top, one.y);
            bottom = std::max(bottom, one.y);
        }
    }

    if (!any) return Box{within.x0, within.y0, within.x0, within.y0};

    const auto clamped = [](double value, int least, int most) { return static_cast<int>(std::clamp(value, static_cast<double>(least), static_cast<double>(most))); };
    const Box reach{clamped(std::floor(left), within.x0, within.x1), clamped(std::floor(top), within.y0, within.y1), clamped(std::ceil(right), within.x0, within.x1), clamped(std::ceil(bottom), within.y0, within.y1)};

    return reach.common(within);
}

// Coverage of nothing over `box`, each pixel's row to be cut into `rows` rows. The cells stay
// from one use to the next, all of them zero again after each.
void Coverage::reset(const Box& box, int rows) {
    box_ = box;
    rows_ = std::max(1, rows);
    stride_ = box.empty() ? 0 : static_cast<std::size_t>(box.wide()) + 2;
    steps_.clear();
    starts_.assign(static_cast<std::size_t>(std::max(0, box.high())) + 1, 0);

    const std::size_t cells = stride_ * static_cast<std::size_t>(band * rows_);

    if (cells_.size() < cells) cells_.resize(cells, 0.0F);

    if (notes_.size() < cells) notes_.resize(cells);

    if (marks_.size() < stride_ * band) marks_.resize(stride_ * band, 0);

    spans_.assign(band, std::pair<std::uint32_t, std::uint32_t>{std::numeric_limits<std::uint32_t>::max(), 0});

    windings_.assign(static_cast<std::size_t>(rows_), 0.0);
}

// The coverage of `lines` (each closed back to its start) by `rule`, band by band of the box, so
// that the cut rows of a large raster never stand in memory all at once. The edges are sorted into
// the bands their rows reach first (a counting sort), so that a band sees only its own. The work
// goes with the cut rows the edges cross within the box; past `most_crossings` of them (a pile of
// shapes over the whole raster, such as tens of thousands of wide dashes) it is Trouble full,
// before any is added.
void Coverage::covered(const std::vector<flattening::Line>& lines, Rule rule) {
    if (box_.empty()) return;

    const int bands = (box_.high() + band - 1) / band;
    const auto band_of = [this, bands](double y) { return static_cast<int>(std::clamp(std::floor((y - box_.y0) / band), 0.0, static_cast<double>(bands - 1))); };

    edges_.clear();

    for (const flattening::Line& line : lines) {
        const std::size_t count = line.points.size();

        if (count < 2) continue;

        for (std::size_t at = 0; at < count; ++at) {
            const Point from = line.points[at];
            const Point to = line.points[(at + 1) % count];

            if (from.y == to.y || std::max(from.y, to.y) <= box_.y0 || std::min(from.y, to.y) >= box_.y1 || std::min(from.x, to.x) >= box_.x1) continue;

            edges_.emplace_back(from, to);
        }
    }

    double crossings = 0;

    for (const auto& [from, to] : edges_) crossings += (std::min(std::max(from.y, to.y), static_cast<double>(box_.y1)) - std::max(std::min(from.y, to.y), static_cast<double>(box_.y0))) * rows_;

    if (!(crossings <= most_crossings)) throw Trouble{Kind::full, "A shape crossing more than " + std::to_string(static_cast<long long>(most_crossings)) + " rows of cells."};

    buckets_.assign(static_cast<std::size_t>(bands) + 1, 0);

    for (const auto& [from, to] : edges_)
        for (int one = band_of(std::min(from.y, to.y)), last = band_of(std::max(from.y, to.y)); one <= last; ++one) ++buckets_[static_cast<std::size_t>(one) + 1];

    for (std::size_t one = 1; one < buckets_.size(); ++one) buckets_[one] += buckets_[one - 1];

    bucketed_.assign(buckets_.back(), 0);

    std::vector<std::size_t> filling(buckets_.begin(), buckets_.end() - 1);

    for (std::size_t index = 0; index < edges_.size(); ++index) {
        const auto& [from, to] = edges_[index];

        for (int one = band_of(std::min(from.y, to.y)), last = band_of(std::max(from.y, to.y)); one <= last; ++one) bucketed_[filling[static_cast<std::size_t>(one)]++] = index;
    }

    for (int one = 0; one < bands; ++one) {
        top_ = one * band;
        high_ = std::min(band, box_.high() - top_) * rows_;

        if (++serial_ == 0) {
            std::fill(notes_.begin(), notes_.end(), Note{});
            serial_ = 1;
        }

        for (std::size_t at = buckets_[static_cast<std::size_t>(one)]; at < buckets_[static_cast<std::size_t>(one) + 1]; ++at) lined(edges_[bucketed_[at]].first, edges_[bucketed_[at]].second);

        resolved(rule);
        crossings_.clear();
    }
}

// The edge from `from` to `to`, in the coordinates of the raster, within the band.
void Coverage::lined(Point from, Point to) {
    if (box_.empty()) return;

    const long double wide = box_.wide();
    const long double high = high_;
    long double ax = static_cast<long double>(from.x) - box_.x0;
    long double ay = (static_cast<long double>(from.y) - box_.y0 - top_) * rows_;
    long double bx = static_cast<long double>(to.x) - box_.x0;
    long double by = (static_cast<long double>(to.y) - box_.y0 - top_) * rows_;
    double direction = 1;

    if (ay == by) return;

    if (ay > by) {
        std::swap(ax, bx);
        std::swap(ay, by);
        direction = -1;
    }

    if (by <= 0 || ay >= high) return;

    if (ay < 0) {
        ax += (0 - ay) * (bx - ax) / (by - ay);
        ay = 0;
    }

    if (by > high) {
        bx = ax + (high - ay) * (bx - ax) / (by - ay);
        by = high;
    }

    long double cuts[4] = {0, 1, 1, 1};
    int count = 1;

    if (bx != ax) {
        for (const long double side : {0.0L, wide}) {
            const long double t = (side - ax) / (bx - ax);

            if (t > 0 && t < 1) cuts[count++] = t;
        }
    }

    cuts[count++] = 1;

    if (count == 4 && cuts[1] > cuts[2]) std::swap(cuts[1], cuts[2]);

    for (int at = 0; at + 1 < count; ++at) {
        const long double t0 = cuts[at];
        const long double t1 = cuts[at + 1];

        if (t1 <= t0) continue;

        long double x0 = ax + (bx - ax) * t0;
        long double x1 = ax + (bx - ax) * t1;
        const long double y0 = at == 0 ? ay : ay + (by - ay) * t0;
        const long double y1 = at + 2 == count ? by : ay + (by - ay) * t1;
        const long double middle = (x0 + x1) / 2;

        if (middle >= wide) continue;

        if (middle <= 0) {
            x0 = x1 = 0;
        } else {
            x0 = std::clamp(x0, 0.0L, wide);
            x1 = std::clamp(x1, 0.0L, wide);
        }

        if (y1 > y0) accumulated(static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1), static_cast<double>(y1), direction);
    }
}

// The edge from (`x0`, `y0`) down to (`x1`, `y1`), within the box, its winding `direction`, added
// to the cells of the rows it crosses.
void Coverage::accumulated(double x0, double y0, double x1, double y1, double direction) {
    const double wide = box_.wide();
    const double slope = (x1 - x0) / (y1 - y0);
    const int first = static_cast<int>(std::floor(y0));
    const int last = std::min(high_, static_cast<int>(std::ceil(y1)));
    double x = x0;

    for (int y = std::max(0, first); y < last; ++y) {
        const double top = std::max(static_cast<double>(y), y0);
        const double bottom = std::min(static_cast<double>(y + 1), y1);

        if (bottom <= top) continue;

        const double next = std::clamp(x0 + slope * (bottom - y0), 0.0, wide);
        const double height = (bottom - top) * direction;
        const double near = std::min(x, next);
        const double far = std::max(x, next);
        const double near_floor = std::floor(near);
        const auto near_cell = static_cast<std::size_t>(near_floor);
        const double far_ceiling = std::ceil(far);
        const auto far_cell = static_cast<std::size_t>(far_ceiling);
        float* const row = cells_.data() + static_cast<std::size_t>(y) * stride_;

        const std::size_t pixel = static_cast<std::size_t>(y / rows_);
        const std::size_t end = std::max(far_cell, near_cell + 1);
        std::uint8_t* const marks = marks_.data() + pixel * stride_;
        auto& [least, most] = spans_[pixel];

        for (std::size_t cell = near_cell; cell <= end; ++cell) marks[cell] |= 1;

        least = std::min(least, static_cast<std::uint32_t>(near_cell));
        most = std::max(most, static_cast<std::uint32_t>(end));

        if (far_cell <= near_cell + 1) {
            const double middle = (x + next) / 2 - near_floor;
            Note& note = notes_[static_cast<std::size_t>(y) * stride_ + near_cell];

            row[near_cell] += static_cast<float>(height - height * middle);
            row[near_cell + 1] += static_cast<float>(height * middle);

            if (x != 0 || next != 0) {
                if (note.stamp != serial_) {
                    note = Note{serial_, -1, static_cast<float>(middle), static_cast<float>(height)};
                } else {
                    if (note.head < 0) {
                        crossings_.push_back(Crossing{note.place, note.height, -1});
                        note.head = static_cast<std::int32_t>(crossings_.size() - 1);
                    }

                    crossings_.push_back(Crossing{static_cast<float>(middle), static_cast<float>(height), note.head});
                    note.head = static_cast<std::int32_t>(crossings_.size() - 1);
                    marks[near_cell] |= 2;
                }
            }
        } else {
            const double per = 1 / (far - near);
            const double near_part = near - near_floor;
            const double first_area = per * (1 - near_part) * (1 - near_part) / 2;
            const double far_part = far - far_ceiling + 1;
            const double last_area = per * far_part * far_part / 2;

            row[near_cell] += static_cast<float>(height * first_area);

            if (far_cell == near_cell + 2) {
                row[near_cell + 1] += static_cast<float>(height * (1 - first_area - last_area));
            } else {
                const double second = per * (1.5 - near_part);

                row[near_cell + 1] += static_cast<float>(height * (second - first_area));

                for (std::size_t cell = near_cell + 2; cell + 1 < far_cell; ++cell) row[cell] += static_cast<float>(height * per);

                const double before_last = second + static_cast<double>(far_cell - near_cell - 3) * per;

                row[far_cell - 1] += static_cast<float>(height * (1 - before_last - last_area));
            }

            row[far_cell] += static_cast<float>(height * last_area);
        }

        x = next;
    }
}

// The coverage by `rule` (through `rule_of`) of a cell of a cut row holding two steps or more
// (noted from `head`), `winding` being its sum: the winding at its left side (the sum less what
// each step adds for the part of the cell past it), then the rule of each stretch between steps,
// by its width. Where no order of the steps could take the winding out of 0 to 1 (or -1 to 0),
// the rule is linear there and the sum is exact; where under nonzero every order keeps it at one
// or more in size, the cell is covered.
template <typename Ruling>
float Coverage::across(std::int32_t head, double winding, Rule rule, const Ruling& rule_of) {
    std::array<std::pair<float, float>, 8> few{};
    std::size_t count = 0;
    double left = winding;
    double up = 0;
    double down = 0;

    sorted_.clear();

    for (std::int32_t at = head; at >= 0; at = crossings_[static_cast<std::size_t>(at)].next) {
        if (count + sorted_.size() >= most_steps) return rule_of(winding);

        const Crossing& crossing = crossings_[static_cast<std::size_t>(at)];
        const std::pair<float, float> one{crossing.place, crossing.height};
        const auto height = static_cast<double>(one.second);

        left -= height * (1 - static_cast<double>(one.first));
        (height > 0 ? up : down) += height;

        if (count < few.size()) {
            std::size_t into = count++;

            for (; into > 0 && few[into - 1].first > one.first; --into) few[into] = few[into - 1];

            few[into] = one;
        } else {
            if (sorted_.empty()) sorted_.assign(few.begin(), few.end());

            sorted_.push_back(one);
        }
    }

    if ((left + down >= 0 && left + up <= 1) || (left + up <= 0 && left + down >= -1)) return rule_of(winding);

    if (rule == Rule::nonzero && (left + down >= 1 || left + up <= -1)) return 1;

    if (!sorted_.empty()) std::sort(sorted_.begin(), sorted_.end());

    const std::span<const std::pair<float, float>> steps = sorted_.empty() ? std::span<const std::pair<float, float>>{few.data(), count} : std::span<const std::pair<float, float>>{sorted_};
    double cover = 0;
    double from = 0;

    for (const auto& [place, height] : steps) {
        cover += static_cast<double>(rule_of(left)) * (static_cast<double>(place) - from);
        left += static_cast<double>(height);
        from = static_cast<double>(place);
    }

    cover += static_cast<double>(rule_of(left)) * (1 - from);

    return static_cast<float>(cover);
}

// The windings of the band turned into steps of coverage by `rule`, a pixel row at a time: the
// columns its cut rows touched (marked as the edges were added, between the least and the most),
// in order, each giving every cut row its winding there (along its steps where it has two or
// more, its mark saying so); a pixel's coverage is the mean of its cut rows' coverages, and stays
// until the next touched column, since an untouched cell changes no winding. Every touched cell
// and mark is set to zero again on the way; the notes of steps stand, a new stamp for each band
// telling the old from the new.
void Coverage::resolved(Rule rule) {
    const auto wide = static_cast<std::uint32_t>(box_.wide());
    const float share = 1.0F / static_cast<float>(rows_);
    const auto rule_of = [rule](double winding) {
        double cover = std::abs(winding);

        if (rule == Rule::evenodd) {
            cover = std::fmod(cover, 2.0);
            cover = cover > 1 ? 2 - cover : cover;
        }

        return static_cast<float>(std::min(cover, 1.0));
    };

    for (int pixel = 0; pixel * rows_ < high_; ++pixel) {
        const std::size_t y = static_cast<std::size_t>(top_ + pixel);
        auto& [least, most] = spans_[static_cast<std::size_t>(pixel)];
        std::uint8_t* const marks = marks_.data() + static_cast<std::size_t>(pixel) * stride_;
        float last = 0;

        std::fill(windings_.begin(), windings_.end(), 0.0);

        for (std::uint32_t column = least; column <= most && least <= most; ++column) {
            if (marks[column] == 0) continue;

            const bool mixed = (marks[column] & 2) != 0;

            marks[column] = 0;

            float cover = 0;

            for (int cut = 0; cut < rows_; ++cut) {
                const std::size_t at = static_cast<std::size_t>(pixel * rows_ + cut) * stride_ + column;
                float& cell = cells_[at];
                double& winding = windings_[static_cast<std::size_t>(cut)];

                winding += cell;
                cell = 0;

                if (mixed && notes_[at].stamp == serial_ && notes_[at].head >= 0) {
                    cover += across(notes_[at].head, winding, rule, rule_of);
                } else {
                    cover += rule_of(winding);
                }
            }

            cover *= share;

            if (cover < 1e-5F) cover = 0;

            if (cover > 1 - 1e-5F) cover = 1;

            if (column < wide && cover != last) {
                steps_.push_back(Step{box_.x0 + static_cast<int>(column), cover});
                last = cover;
            }
        }

        least = std::numeric_limits<std::uint32_t>::max();
        most = 0;

        if (last != 0) steps_.push_back(Step{box_.x1, 0});

        starts_[y + 1] = steps_.size();
    }
}

// The steps of coverage of the row `y` of the raster: from each step's column the coverage is its
// own until the next step; before the first and after the last it is zero.
std::span<const Step> Coverage::steps(int y) const {
    const auto at = static_cast<std::size_t>(y - box_.y0);

    return std::span<const Step>{steps_.data() + starts_[at], starts_[at + 1] - starts_[at]};
}

}
