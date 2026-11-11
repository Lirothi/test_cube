#include "app/game/RaceTrack.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace game
{
    namespace
    {
        using Math::float2;

        // A boat's heading on the plane: 0 = +z, growing to starboard (toward +x).
        float Heading(const float2& v) { return std::atan2(v.x, v.y); }

        float WrapAngle(float a)
        {
            constexpr float kPi = 3.14159265f;
            while (a > kPi) { a -= 2.0f * kPi; }
            while (a < -kPi) { a += 2.0f * kPi; }
            return a;
        }

        // A x = b for a small dense system, by elimination with partial pivoting. The periodic spline's matrix is cyclic
        // tridiagonal; a course has tens of gates, so the plain solve is nothing.
        bool Solve(std::vector<double>& a, std::vector<double>& b, std::size_t n)
        {
            for (std::size_t col = 0; col < n; ++col)
            {
                std::size_t pivot = col;
                for (std::size_t row = col + 1; row < n; ++row)
                {
                    if (std::fabs(a[row * n + col]) > std::fabs(a[pivot * n + col])) { pivot = row; }
                }
                if (std::fabs(a[pivot * n + col]) < 1e-12) { return false; }
                if (pivot != col)
                {
                    for (std::size_t k = 0; k < n; ++k) { std::swap(a[col * n + k], a[pivot * n + k]); }
                    std::swap(b[col], b[pivot]);
                }
                for (std::size_t row = col + 1; row < n; ++row)
                {
                    const double f = a[row * n + col] / a[col * n + col];
                    if (f == 0.0) { continue; }
                    for (std::size_t k = col; k < n; ++k) { a[row * n + k] -= f * a[col * n + k]; }
                    b[row] -= f * b[col];
                }
            }
            for (std::size_t i = n; i-- > 0;)
            {
                double sum = b[i];
                for (std::size_t k = i + 1; k < n; ++k) { sum -= a[i * n + k] * b[k]; }
                b[i] = sum / a[i * n + i];
            }
            return true;
        }
    }

    namespace
    {
        // A closed C2 cubic spline through points, its parameter the chord's length: the curve of least bending through
        // them. The second derivatives at the knots solve a cyclic tridiagonal system.
        class ClosedSpline
        {
        public:
            explicit ClosedSpline(const std::vector<float2>& knots)
                : knots_(knots)
            {
                const std::size_t n = knots_.size();
                chord_.resize(n);
                for (std::size_t i = 0; i < n; ++i) { chord_[i] = std::max(static_cast<double>((knots_[(i + 1) % n] - knots_[i]).Length()), 0.05); }
                for (int axis = 0; axis < 2; ++axis)
                {
                    std::vector<double> matrix(n * n, 0.0);
                    std::vector<double>& rhs = second_[axis];
                    rhs.assign(n, 0.0);
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        const std::size_t prev = (i + n - 1) % n;
                        const std::size_t next = (i + 1) % n;
                        matrix[i * n + prev] += chord_[prev];
                        matrix[i * n + i] += 2.0 * (chord_[prev] + chord_[i]);
                        matrix[i * n + next] += chord_[i];
                        rhs[i] = 6.0 * ((Value(next, axis) - Value(i, axis)) / chord_[i] - (Value(i, axis) - Value(prev, axis)) / chord_[prev]);
                    }
                    if (n < 3 || !Solve(matrix, rhs, n)) { rhs.assign(n, 0.0); } // a polygon: still a line
                }
            }

            double Chord(std::size_t i) const { return chord_[i]; }

            // The point `u` along piece i (0 .. Chord(i)), which runs from knot i to the next.
            float2 At(std::size_t i, double u) const
            {
                const std::size_t next = (i + 1) % knots_.size();
                const double len = chord_[i];
                const double rest = len - u;
                float2 out;
                for (int axis = 0; axis < 2; ++axis)
                {
                    const double m0 = second_[axis][i];
                    const double m1 = second_[axis][next];
                    const double v = m0 * rest * rest * rest / (6.0 * len) + m1 * u * u * u / (6.0 * len) + (Value(i, axis) / len - m0 * len / 6.0) * rest +
                                     (Value(next, axis) / len - m1 * len / 6.0) * u;
                    (axis == 0 ? out.x : out.y) = static_cast<float>(v);
                }
                return out;
            }

        private:
            double Value(std::size_t i, int axis) const { return static_cast<double>(axis == 0 ? knots_[i].x : knots_[i].y); }

            std::vector<float2> knots_;
            std::vector<double> chord_;
            std::vector<double> second_[2];
        };
    }

    bool RaceTrack::Build(const RaceCourse& course, const std::string& track, std::string& report)
    {
        gates_.clear();
        report.clear();
        std::map<int, std::vector<float2>> byNumber;
        for (const RaceMark& mark : course.marks)
        {
            if (mark.track == track) { byNumber[mark.gate].emplace_back(mark.position.x, mark.position.z); }
        }
        std::string left;
        for (const auto& [number, marks] : byNumber)
        {
            if (marks.size() != 2 || (marks[1] - marks[0]).Length() < 1.0f)
            {
                left += (left.empty() ? "" : ", ") + std::to_string(number) + " (" + std::to_string(marks.size()) + " marks)";
                continue;
            }
            RaceGate gate;
            gate.number = number;
            gate.a = marks[0];
            gate.b = marks[1];
            gate.center = (gate.a + gate.b) * 0.5f;
            gate.halfWidth = (gate.b - gate.a).Length() * 0.5f;
            gate.across = (gate.b - gate.a).Normalized();
            gates_.push_back(gate);
        }
        if (!left.empty()) { report = "gates left out: " + left + ". "; }
        if (gates_.size() < 3)
        {
            report += "a course needs three gates of two marks each, this one has " + std::to_string(gates_.size());
            gates_.clear();
            return false;
        }
        // The way through a gate is square to its marks, on the side the course runs to; its marks are then put port
        // (a) and starboard (b) of that way.
        const std::size_t n = gates_.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            RaceGate& gate = gates_[i];
            const float2 run = gates_[(i + 1) % n].center - gates_[(i + n - 1) % n].center;
            float2 along(-gate.across.y, gate.across.x);
            if (along.Dot(run) < 0.0f) { along = along * -1.0f; }
            gate.along = along;
            const float2 starboard(along.y, -along.x);
            if (gate.across.Dot(starboard) < 0.0f)
            {
                std::swap(gate.a, gate.b);
                gate.across = gate.across * -1.0f;
            }
        }
        report += std::to_string(n) + " gates";
        return true;
    }

    RaceStart StartOf(const RaceTrack& track)
    {
        RaceStart start;
        if (!track.Valid()) { return start; }
        // From 8 m to 56 m before the start line, a little narrower than gate 1; the rows 16 m and 40 m before the line
        // (a boat is 7-10 m long), the two of a row 0.55 of the half width to either side.
        const RaceGate& gate = track.Gates().front();
        start.along = gate.along;
        start.starboard = gate.across;
        start.halfLength = 24.0f;
        start.halfWidth = std::max(gate.halfWidth - 2.0f, 6.0f);
        start.center = gate.center - gate.along * (8.0f + start.halfLength);
        const float abreast = 0.55f * start.halfWidth;
        for (int i = 0; i < 4; ++i)
        {
            start.slots[i] = gate.center - gate.along * (i < 2 ? 16.0f : 40.0f) + gate.across * ((i & 1) ? abreast : -abreast);
        }
        return start;
    }

    void RaceLine::Clear()
    {
        points_.clear();
        curvature_.clear();
        speed_.clear();
        gateStation_.clear();
        length_ = 0.0f;
        step_ = kStep;
    }

    void RaceLine::Build(const std::vector<RaceGate>& gates, float margin)
    {
        Clear();
        const std::size_t n = gates.size();
        if (n < 3) { return; }

        // ---- where the line crosses each gate: an offset along its marks, as far as the margin lets it ----
        // The polygon through the crossing points is made as straight as it can be: the sum over its corners of
        // |K_i|^2 w_i, K_i the discrete curvature vector a_i P_(i-1) + b_i P_i + c_i P_(i+1) (the second difference on
        // uneven spacing), w_i the length the corner stands for. P_i = C_i + o_i A_i is linear in its offset, so the
        // offset that minimises the three terms P_i is in has a closed form; clamped, swept until it stops moving.
        std::vector<float> h(n), ka(n), kb(n), kc(n), weight(n), offset(n, 0.0f), limit(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            h[i] = std::max((gates[(i + 1) % n].center - gates[i].center).Length(), 1.0f);
            limit[i] = std::max(gates[i].halfWidth - margin, 0.0f);
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            const float before = h[(i + n - 1) % n];
            const float after = h[i];
            ka[i] = 2.0f / (before * (before + after));
            kc[i] = 2.0f / (after * (before + after));
            kb[i] = -(ka[i] + kc[i]);
            weight[i] = 0.5f * (before + after);
        }
        const auto point = [&](std::size_t i) { return gates[i].center + gates[i].across * offset[i]; };
        const auto bend = [&](std::size_t i) { return point((i + n - 1) % n) * ka[i] + point(i) * kb[i] + point((i + 1) % n) * kc[i]; };
        for (int sweep = 0; sweep < 300; ++sweep)
        {
            float moved = 0.0f;
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::size_t prev = (i + n - 1) % n;
                const std::size_t next = (i + 1) % n;
                const float was = offset[i];
                offset[i] = 0.0f;
                const float2 across = gates[i].across;
                // P_i enters K_prev with kc[prev], K_i with kb[i], K_next with ka[next].
                const float slope = weight[prev] * kc[prev] * bend(prev).Dot(across) + weight[i] * kb[i] * bend(i).Dot(across) +
                                    weight[next] * ka[next] * bend(next).Dot(across);
                const float stiffness = weight[prev] * kc[prev] * kc[prev] + weight[i] * kb[i] * kb[i] + weight[next] * ka[next] * ka[next];
                offset[i] = std::clamp(stiffness > 1e-12f ? -slope / stiffness : 0.0f, -limit[i], limit[i]);
                moved = std::max(moved, std::fabs(offset[i] - was));
            }
            if (moved < 0.005f) { break; }
        }

        // ---- a first line: the closed spline through the crossing points, cut into pieces of some 10 m ----
        std::vector<float2> knots(n);
        for (std::size_t i = 0; i < n; ++i) { knots[i] = point(i); }
        std::vector<float2> band;
        std::vector<std::size_t> pinned(n, 0); // the band's point that lies on gate i's line
        {
            const ClosedSpline first(knots);
            for (std::size_t i = 0; i < n; ++i)
            {
                pinned[i] = band.size();
                const int pieces = std::max(static_cast<int>(std::lround(first.Chord(i) / 10.0)), 2);
                for (int k = 0; k < pieces; ++k) { band.push_back(first.At(i, first.Chord(i) * k / pieces)); }
            }
        }

        // ---- the elastic band: every point where the bending of the polygon through its four neighbours is least ----
        // (p[i-2] - 4 p[i-1] + 6 p[i] - 4 p[i+1] + p[i+2] = 0, Gauss-Seidel), a gate's point kept ON its gate's line and
        // inside its window. The spline through the gates' points alone is the least-bending curve through THOSE points,
        // not through the gates: between them its curvature peaked -- 58 m of radius in a turn whose gates stand on 85 m
        // (2026-11-10: police_boat hard over and 16 m wide of it) -- because where it crossed each gate had been chosen
        // for the polygon, not for the curve.
        {
            const std::size_t m = band.size();
            std::vector<int> gateOf(m, -1);
            for (std::size_t i = 0; i < n; ++i) { gateOf[pinned[i]] = static_cast<int>(i); }
            for (int sweep = 0; sweep < 4000; ++sweep)
            {
                float moved = 0.0f;
                for (std::size_t j = 0; j < m; ++j)
                {
                    const float2 was = band[j];
                    float2 at = ((band[(j + m - 1) % m] + band[(j + 1) % m]) * 4.0f - (band[(j + m - 2) % m] + band[(j + 2) % m])) / 6.0f;
                    if (gateOf[j] >= 0)
                    {
                        const RaceGate& gate = gates[static_cast<std::size_t>(gateOf[j])];
                        const float window = limit[static_cast<std::size_t>(gateOf[j])];
                        at = gate.center + gate.across * std::clamp((at - gate.center).Dot(gate.across), -window, window);
                    }
                    band[j] = at;
                    moved = std::max(moved, (at - was).Length());
                }
                if (moved < 2e-4f) { break; }
            }
        }

        // ---- the line: the closed spline through the band; finely, to measure it, then every kStep metres of its length ----
        const ClosedSpline spline(band);
        std::vector<float2> fine;
        std::vector<float> fineLength;
        std::vector<float> bandLength(band.size(), 0.0f);
        float total = 0.0f;
        for (std::size_t i = 0; i < band.size(); ++i)
        {
            const int pieces = std::max(static_cast<int>(std::ceil(spline.Chord(i) / 0.5)), 2);
            for (int k = 0; k < pieces; ++k)
            {
                const float2 at = spline.At(i, spline.Chord(i) * k / pieces);
                if (!fine.empty()) { total += (at - fine.back()).Length(); }
                if (k == 0) { bandLength[i] = total; }
                fine.push_back(at);
                fineLength.push_back(total);
            }
        }
        total += (fine.front() - fine.back()).Length();
        if (!(total > 8.0f * kStep)) { return; }
        const std::size_t count = std::max<std::size_t>(static_cast<std::size_t>(std::lround(total / kStep)), 8);
        step_ = total / static_cast<float>(count);
        length_ = total;
        points_.resize(count);
        std::size_t cursor = 0;
        for (std::size_t s = 0; s < count; ++s)
        {
            const float want = step_ * static_cast<float>(s);
            while (cursor + 1 < fine.size() && fineLength[cursor + 1] <= want) { ++cursor; }
            const float2& from = fine[cursor];
            const bool last = cursor + 1 >= fine.size();
            const float2& to = last ? fine.front() : fine[cursor + 1];
            const float span = (last ? total : fineLength[cursor + 1]) - fineLength[cursor];
            points_[s] = float2::Lerp(from, to, span > 1e-5f ? std::clamp((want - fineLength[cursor]) / span, 0.0f, 1.0f) : 0.0f);
        }
        gateStation_.resize(n);
        for (std::size_t i = 0; i < n; ++i) { gateStation_[i] = bandLength[pinned[i]]; }

        // ---- how hard it turns: the heading's change from one piece to the next, lightly smoothed ----
        std::vector<float> raw(count);
        for (std::size_t s = 0; s < count; ++s)
        {
            const float2 in = points_[s] - points_[(s + count - 1) % count];
            const float2 out = points_[(s + 1) % count] - points_[s];
            raw[s] = WrapAngle(Heading(out) - Heading(in)) / step_;
        }
        curvature_.resize(count);
        for (std::size_t s = 0; s < count; ++s)
        {
            curvature_[s] = (raw[(s + count - 2) % count] + raw[(s + count - 1) % count] + raw[s] + raw[(s + 1) % count] + raw[(s + 2) % count]) * 0.2f;
        }
        speed_.assign(count, 0.0f);
    }

    void RaceLine::Profile(const std::function<float(float)>& speedFor, float brakeAccel)
    {
        const std::size_t count = points_.size();
        if (count < 8) { return; }
        speed_.resize(count);
        for (std::size_t s = 0; s < count; ++s) { speed_[s] = speedFor(std::fabs(curvature_[s])); }
        // Backwards, twice round: a sample is no faster than braking from it still makes the next.
        for (std::size_t k = 2 * count; k-- > 0;)
        {
            const std::size_t s = k % count;
            const float next = speed_[(s + 1) % count];
            speed_[s] = std::min(speed_[s], std::sqrt(next * next + 2.0f * std::max(brakeAccel, 0.05f) * step_));
        }
    }

    float RaceLine::SlowestSpeed() const
    {
        float slowest = 0.0f;
        for (std::size_t s = 0; s < speed_.size(); ++s) { slowest = s == 0 ? speed_[s] : std::min(slowest, speed_[s]); }
        return slowest;
    }

    float RaceLine::TightestRadius(Math::float2* where, float* station) const
    {
        float most = 0.0f;
        std::size_t at = 0;
        for (std::size_t i = 0; i < curvature_.size(); ++i)
        {
            if (std::fabs(curvature_[i]) > most)
            {
                most = std::fabs(curvature_[i]);
                at = i;
            }
        }
        if (where && at < points_.size()) { *where = points_[at]; }
        if (station) { *station = static_cast<float>(at) * step_; }
        return most > 0.0f ? 1.0f / most : 0.0f;
    }

    RaceLine::Sample RaceLine::At(float station) const
    {
        Sample out;
        const std::size_t count = points_.size();
        if (count < 8) { return out; }
        float s = std::fmod(station, length_);
        if (s < 0.0f) { s += length_; }
        const float at = s / step_;
        const std::size_t i = std::min(static_cast<std::size_t>(at), count - 1);
        const std::size_t j = (i + 1) % count;
        const float t = std::clamp(at - static_cast<float>(i), 0.0f, 1.0f);
        out.point = float2::Lerp(points_[i], points_[j], t);
        out.tangent = (points_[j] - points_[i]).Normalized();
        out.curvature = curvature_[i] + (curvature_[j] - curvature_[i]) * t;
        out.speed = speed_.size() == count ? speed_[i] + (speed_[j] - speed_[i]) * t : 0.0f;
        return out;
    }

    float RaceLine::Nearest(const Math::float2& point) const { return Nearest(point, 0.0f, 0.0f, length_); }

    float RaceLine::Nearest(const Math::float2& point, float from, float back, float ahead) const
    {
        const std::size_t count = points_.size();
        if (count < 8) { return 0.0f; }
        const long first = static_cast<long>(std::floor((from - back) / step_));
        const long pieces = std::min(static_cast<long>(std::ceil((back + ahead) / step_)) + 1, static_cast<long>(count));
        float best = from;
        float bestDistance = 3.4e38f;
        for (long k = 0; k < pieces; ++k)
        {
            const long index = ((first + k) % static_cast<long>(count) + static_cast<long>(count)) % static_cast<long>(count);
            const float2& a = points_[static_cast<std::size_t>(index)];
            const float2& b = points_[static_cast<std::size_t>((index + 1) % static_cast<long>(count))];
            const float2 ab = b - a;
            const float len2 = ab.Dot(ab);
            const float t = len2 > 1e-8f ? std::clamp((point - a).Dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
            const float2 off = point - (a + ab * t);
            const float distance = off.Dot(off);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = (static_cast<float>(index) + t) * step_;
            }
        }
        return best;
    }
}
