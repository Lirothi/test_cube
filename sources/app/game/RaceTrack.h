#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "app/game/RaceCourse.h"
#include "core/math/Math.h"

// The geometry of a race course (game::RaceCourse): its gates in order, and the LINE a boat runs through them.
// Everything here is on the water's plane: a float2 is (x, z).
namespace game
{
    struct RaceGate
    {
        int number = 0;
        Math::float2 a;      // its two marks
        Math::float2 b;
        Math::float2 center;
        Math::float2 across; // unit, a -> b
        Math::float2 along;  // unit, the way through it: square to the marks, toward the next gate
        float halfWidth = 0.0f;
    };

    class RaceTrack
    {
    public:
        // The gates out of the course's marks, in the order of their numbers. False (and why, in `report`) when fewer than
        // three whole gates come out: a number with one mark or more than two is left out and named.
        bool Build(const RaceCourse& course, std::string& report);
        void Clear() { gates_.clear(); }
        bool Valid() const { return gates_.size() >= 3; }
        const std::vector<RaceGate>& Gates() const { return gates_; }

    private:
        std::vector<RaceGate> gates_;
    };

    // The line one boat runs: through every gate no nearer than `margin` to a mark, as straight as the gates let it be
    // (the offsets inside the gates minimise the bending of the polygon through them), a closed C2 cubic spline through
    // those points, sampled every kStep metres -- and how fast each sample may be taken.
    class RaceLine
    {
    public:
        static constexpr float kStep = 2.0f;

        struct Sample
        {
            Math::float2 point;
            Math::float2 tangent; // unit
            float curvature = 0.0f; // 1/m, positive = turning to starboard (clockwise seen from above)
            float speed = 0.0f;     // m/s, the profile's
        };

        void Build(const std::vector<RaceGate>& gates, float margin);
        // The speed each sample may be taken at: what `speedFor` says of its curvature (1/m, unsigned), and what braking
        // at brakeAccel from it still makes the next slow place.
        void Profile(const std::function<float(float)>& speedFor, float brakeAccel);
        void Clear();
        bool Valid() const { return points_.size() >= 8; }
        float Length() const { return length_; }
        // `station` is metres along the line from gate 1, any value (it wraps).
        Sample At(float station) const;
        // The station nearest to the point: on the whole line, or within [from - back, from + ahead] (a boat's own place
        // moves on and must not jump to another leg that happens to pass near).
        float Nearest(const Math::float2& point) const;
        float Nearest(const Math::float2& point, float from, float back, float ahead) const;
        // Where the line crosses gate i (the index in RaceTrack::Gates).
        float GateStation(std::size_t gate) const { return gate < gateStation_.size() ? gateStation_[gate] : 0.0f; }
        const std::vector<Math::float2>& Points() const { return points_; }
        float StepLength() const { return step_; }
        float SpeedAt(std::size_t sample) const { return sample < speed_.size() ? speed_[sample] : 0.0f; }
        float SlowestSpeed() const;

    private:
        std::vector<Math::float2> points_;
        std::vector<float> curvature_;
        std::vector<float> speed_;
        std::vector<float> gateStation_;
        float step_ = kStep;
        float length_ = 0.0f;
    };
}
