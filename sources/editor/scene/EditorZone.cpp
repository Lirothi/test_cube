#include "editor/scene/EditorZone.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>

namespace
{
    std::string Lowered(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    float NumberOr(const nlohmann::json& properties, const char* key, float fallback)
    {
        const auto it = properties.find(key);
        return it != properties.end() && it->is_number() ? it->get<float>() : fallback;
    }

    // How finely a span between two control points is cut. Twelve is enough that a bend
    // reads as a curve rather than as a fan of chords, and small enough that the point-in-
    // polygon test over a long coastline stays trivial.
    constexpr int kSegmentsPerSpan = 12;

    // Catmull-Rom, which passes THROUGH its control points rather than being pulled towards
    // them. That is the property that matters here: a dragged point must end up on the
    // curve, or the handles stop meaning what they look like they mean.
    Math::float3 CatmullRom(const Math::float3& p0, const Math::float3& p1,
        const Math::float3& p2, const Math::float3& p3, float t)
    {
        const float t2 = t * t;
        const float t3 = t2 * t;
        const auto axis = [&](float a0, float a1, float a2, float a3)
        {
            return 0.5f * ((2.0f * a1) + (-a0 + a2) * t +
                (2.0f * a0 - 5.0f * a1 + 4.0f * a2 - a3) * t2 +
                (-a0 + 3.0f * a1 - 3.0f * a2 + a3) * t3);
        };
        return Math::float3(axis(p0.x, p1.x, p2.x, p3.x), axis(p0.y, p1.y, p2.y, p3.y),
            axis(p0.z, p1.z, p2.z, p3.z));
    }

    std::vector<Math::float3> Tessellate(const std::vector<Math::float3>& points, bool closed)
    {
        std::vector<Math::float3> curve;
        const int count = static_cast<int>(points.size());
        if (count < 2)
        {
            return points;
        }
        const auto at = [&](int index) -> const Math::float3&
        {
            if (closed)
            {
                return points[static_cast<std::size_t>(((index % count) + count) % count)];
            }
            // Open: the ends repeat themselves, so the curve starts and finishes at the
            // first and last control points instead of overshooting past them.
            return points[static_cast<std::size_t>(std::clamp(index, 0, count - 1))];
        };
        const int spans = closed ? count : count - 1;
        curve.reserve(static_cast<std::size_t>(spans) * kSegmentsPerSpan + 1);
        for (int span = 0; span < spans; ++span)
        {
            for (int step = 0; step < kSegmentsPerSpan; ++step)
            {
                const float t = static_cast<float>(step) / static_cast<float>(kSegmentsPerSpan);
                curve.push_back(CatmullRom(at(span - 1), at(span), at(span + 1),
                    at(span + 2), t));
            }
        }
        if (!closed)
        {
            curve.push_back(points.back());
        }
        return curve;
    }
}

namespace editorzone
{
    bool FromObject(const EditorObject& object, Zone& outZone)
    {
        if (object.type != kTypeName || !object.properties.is_object())
        {
            return false;
        }
        outZone.id = object.id;
        outZone.name = object.name;
        outZone.centre = object.transform.position;
        outZone.yawDeg = object.transform.rotationDeg.y;

        const auto shapeIt = object.properties.find("shape");
        const std::string shape = shapeIt != object.properties.end() && shapeIt->is_string()
            ? shapeIt->get<std::string>() : "circle";
        outZone.shape = shape == "rect" ? Shape::Rect
            : shape == "spline" ? Shape::Spline : Shape::Circle;

        const float radius = std::max(0.1f, NumberOr(object.properties, "radius", 25.0f));
        const float halfX = std::max(0.1f, NumberOr(object.properties, "halfX", radius));
        const float halfZ = std::max(0.1f, NumberOr(object.properties, "halfZ", radius));
        // Scale folded in here, once, so nothing downstream has to remember to. Y is
        // ignored on purpose: a zone is a footprint, and there is no such thing as a tall
        // one -- letting the Y handle change the shape would be a control that lies.
        outZone.halfX = std::max(0.1f, halfX * std::fabs(object.transform.scale.x));
        outZone.halfZ = std::max(0.1f, halfZ * std::fabs(object.transform.scale.z));

        if (outZone.shape == Shape::Spline)
        {
            outZone.closed = object.properties.value("closed", false);
            const auto fillIt = object.properties.find("fill");
            outZone.fill = (fillIt != object.properties.end() && fillIt->is_string() &&
                fillIt->get<std::string>() == "inside") ? Fill::Inside : Fill::Along;
            const std::string plane = object.properties.value("plane", std::string("xz"));
            outZone.plane = plane == "xy" ? Plane::XY : plane == "zy" ? Plane::ZY : Plane::XZ;
            // The band's width scales with the object too, by the smaller axis: widening a
            // path by stretching it sideways would be surprising, and taking the larger
            // would make a squashed spline fatter than it looks.
            const float widthScale = std::min(std::fabs(object.transform.scale.x),
                std::fabs(object.transform.scale.z));
            outZone.halfWidth = std::max(0.25f,
                NumberOr(object.properties, "halfWidth", 8.0f) * widthScale);
            // Points carried into world space here, once, so nothing downstream repeats the
            // transform and gets it subtly different.
            const float yaw = outZone.yawDeg * 0.017453293f;
            const float c = std::cos(yaw);
            const float s = std::sin(yaw);
            for (const Math::float3& local : LocalPoints(object))
            {
                const float sx = local.x * object.transform.scale.x;
                const float sz = local.z * object.transform.scale.z;
                // Y carried through as well: the points are 3D, so a path can climb.
                outZone.points.push_back(Math::float3(outZone.centre.x + sx * c - sz * s,
                    outZone.centre.y + local.y * object.transform.scale.y,
                    outZone.centre.z + sx * s + sz * c));
            }
            // A spline with fewer than two points is a point, and everything below divides
            // by a segment length. Degrade to a circle of the band's width rather than
            // producing a zone that samples nothing and reports no reason.
            if (outZone.points.size() < 2)
            {
                outZone.shape = Shape::Circle;
                outZone.halfX = outZone.halfWidth;
                outZone.halfZ = outZone.halfWidth;
                outZone.points.clear();
            }
            else
            {
                outZone.curve = Tessellate(outZone.points, outZone.closed);
            }
        }
        return true;
    }

    std::vector<Math::float3> LocalPoints(const EditorObject& object)
    {
        std::vector<Math::float3> points;
        if (!object.properties.is_object())
        {
            return points;
        }
        const auto it = object.properties.find("points");
        if (it == object.properties.end() || !it->is_array())
        {
            return points;
        }
        // [[x, z], ...] -- two numbers, because a zone is a footprint and a height here
        // would be a number nothing reads.
        // [x, y, z]. Two numbers are read as [x, z] with y = 0, because that is what the
        // first version wrote and a level saved then must still open.
        for (const nlohmann::json& entry : *it)
        {
            if (!entry.is_array() || entry.size() < 2 || !entry[0].is_number() ||
                !entry[1].is_number())
            {
                continue;
            }
            if (entry.size() >= 3 && entry[2].is_number())
            {
                points.push_back(Math::float3(entry[0].get<float>(), entry[1].get<float>(),
                    entry[2].get<float>()));
            }
            else
            {
                points.push_back(Math::float3(entry[0].get<float>(), 0.0f,
                    entry[1].get<float>()));
            }
        }
        return points;
    }

    void StoreLocalPoints(EditorObject& object, const std::vector<Math::float3>& points)
    {
        nlohmann::json array = nlohmann::json::array();
        for (const Math::float3& point : points)
        {
            array.push_back(nlohmann::json::array({ point.x, point.y, point.z }));
        }
        object.properties["points"] = std::move(array);
    }

    namespace
    {
        // Squared distance from p to the segment ab, in the XZ plane, plus where along it.
        float DistanceSqToSegment(float px, float pz, const Math::float3& a,
            const Math::float3& b)
        {
            const float abx = b.x - a.x;
            const float abz = b.z - a.z;
            const float lengthSq = abx * abx + abz * abz;
            if (lengthSq <= 1e-6f)
            {
                const float dx = px - a.x;
                const float dz = pz - a.z;
                return dx * dx + dz * dz;
            }
            float t = ((px - a.x) * abx + (pz - a.z) * abz) / lengthSq;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float dx = px - (a.x + abx * t);
            const float dz = pz - (a.z + abz * t);
            return dx * dx + dz * dz;
        }
    }

    std::vector<Zone> Collect(const EditorSceneDocument& document)
    {
        std::vector<Zone> zones;
        for (const EditorObject& object : document.Objects())
        {
            Zone zone;
            if (FromObject(object, zone))
            {
                zones.push_back(zone);
            }
        }
        return zones;
    }

    bool Find(const EditorSceneDocument& document,
        const std::string& needle,
        Zone& outZone,
        std::string& outWhyNot)
    {
        const std::vector<Zone> zones = Collect(document);
        if (zones.empty())
        {
            outWhyNot = "This level has no zones. Create one from Create > Zone.";
            return false;
        }
        const std::string wanted = Lowered(needle);
        std::vector<const Zone*> hits;
        for (const Zone& zone : zones)
        {
            if (Lowered(zone.name).find(wanted) != std::string::npos)
            {
                hits.push_back(&zone);
            }
        }
        if (hits.empty())
        {
            outWhyNot = "No zone matches '" + needle + "'. This level has: ";
            for (std::size_t index = 0; index < zones.size(); ++index)
            {
                outWhyNot += (index ? ", " : "") + zones[index].name;
            }
            return false;
        }
        if (hits.size() > 1)
        {
            // Naming which ones, because "ambiguous" without the candidates is a dead end.
            outWhyNot = "'" + needle + "' matches more than one zone: ";
            for (std::size_t index = 0; index < hits.size(); ++index)
            {
                outWhyNot += (index ? ", " : "") + hits[index]->name;
            }
            return false;
        }
        outZone = *hits.front();
        return true;
    }

    float PointRadius(const EditorObject& object)
    {
        return std::max(0.05f, NumberOr(object.properties, "pointRadius", 1.5f));
    }

    void PlaneAxes(Plane plane, int& outU, int& outV)
    {
        switch (plane)
        {
        case Plane::XY: outU = 0; outV = 1; return;
        case Plane::ZY: outU = 2; outV = 1; return;
        case Plane::XZ:
        default:        outU = 0; outV = 2; return;
        }
    }

    namespace
    {
        float Axis(const Math::float3& v, int index)
        {
            return index == 0 ? v.x : (index == 1 ? v.y : v.z);
        }

        // Squared distance from (pu, pv) to the segment ab, measured in the zone's plane.
        float DistanceSqInPlane(float pu, float pv, const Math::float3& a,
            const Math::float3& b, int u, int v)
        {
            const float abu = Axis(b, u) - Axis(a, u);
            const float abv = Axis(b, v) - Axis(a, v);
            const float lengthSq = abu * abu + abv * abv;
            if (lengthSq <= 1e-6f)
            {
                const float du = pu - Axis(a, u);
                const float dv = pv - Axis(a, v);
                return du * du + dv * dv;
            }
            float t = ((pu - Axis(a, u)) * abu + (pv - Axis(a, v)) * abv) / lengthSq;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float du = pu - (Axis(a, u) + abu * t);
            const float dv = pv - (Axis(a, v) + abv * t);
            return du * du + dv * dv;
        }
    }

    bool Contains(const Zone& zone, const Math::float3& position)
    {
        if (zone.shape == Shape::Spline)
        {
            // Measured in the zone's OWN plane, so a spline running up a cliff is tested
            // against the wall it lives on rather than against its shadow on the ground.
            int u = 0;
            int v = 2;
            PlaneAxes(zone.plane, u, v);
            const float pu = Axis(position, u);
            const float pv = Axis(position, v);
            if (zone.fill == Fill::Inside)
            {
                // Even-odd crossing count against the tessellated curve, closed for the
                // test whether or not the spline says it is closed: an open line encloses
                // nothing, and refusing over a flag nobody set would be pedantry.
                bool inside = false;
                const std::size_t count = zone.curve.size();
                for (std::size_t i = 0, j = count - 1; i < count; j = i++)
                {
                    const float au = Axis(zone.curve[i], u);
                    const float av = Axis(zone.curve[i], v);
                    const float bu = Axis(zone.curve[j], u);
                    const float bv = Axis(zone.curve[j], v);
                    if ((av > pv) != (bv > pv) &&
                        pu < (bu - au) * (pv - av) / (bv - av) + au)
                    {
                        inside = !inside;
                    }
                }
                return inside;
            }
            // Along: within halfWidth of the curve.
            const float limitSq = zone.halfWidth * zone.halfWidth;
            const std::size_t spans = zone.closed ? zone.curve.size() : zone.curve.size() - 1;
            for (std::size_t index = 0; index < spans; ++index)
            {
                const Math::float3& a = zone.curve[index];
                const Math::float3& b = zone.curve[(index + 1) % zone.curve.size()];
                if (DistanceSqInPlane(pu, pv, a, b, u, v) <= limitSq)
                {
                    return true;
                }
            }
            return false;
        }

        const float dx = position.x - zone.centre.x;
        const float dz = position.z - zone.centre.z;
        // Into the zone's own frame, so a rotated one tests as its actual shape rather than
        // as its axis-aligned bounding box.
        const float yaw = zone.yawDeg * 0.017453293f;
        const float c = std::cos(-yaw);
        const float s = std::sin(-yaw);
        const float localX = dx * c - dz * s;
        const float localZ = dx * s + dz * c;
        if (zone.shape == Shape::Circle)
        {
            const float nx = localX / zone.halfX;
            const float nz = localZ / zone.halfZ;
            return nx * nx + nz * nz <= 1.0f;
        }
        return std::fabs(localX) <= zone.halfX && std::fabs(localZ) <= zone.halfZ;
    }

    Math::float3 SamplePoint(const Zone& zone, float unit01A, float unit01B)
    {
        if (zone.shape == Shape::Spline)
        {
            int u = 0;
            int v = 2;
            PlaneAxes(zone.plane, u, v);
            const auto compose = [&](float valueU, float valueV, float other)
            {
                float xyz[3] = { other, other, other };
                xyz[u] = valueU;
                xyz[v] = valueV;
                // The third axis is the centre's: for a ground zone that is the height the
                // caller is about to overwrite with a ground probe anyway, and for a wall
                // zone it is the wall's depth.
                const int w = 3 - u - v;
                xyz[w] = Axis(zone.centre, w);
                return Math::float3(xyz[0], xyz[1], xyz[2]);
            };
            if (zone.fill == Fill::Inside)
            {
                // Rejection sampling inside the curve's bounding box. The caller's rejection
                // loop already discards water and slopes and retries, so a miss here costs
                // one more attempt out of the same budget -- and any cleverer scheme
                // (triangulation, scanlines) would be a lot of code for a shape whose area
                // is rarely a small fraction of its box.
                float minU = Axis(zone.curve.front(), u);
                float maxU = minU;
                float minV = Axis(zone.curve.front(), v);
                float maxV = minV;
                for (const Math::float3& p : zone.curve)
                {
                    minU = std::min(minU, Axis(p, u));
                    maxU = std::max(maxU, Axis(p, u));
                    minV = std::min(minV, Axis(p, v));
                    maxV = std::max(maxV, Axis(p, v));
                }
                return compose(minU + unit01A * (maxU - minU), minV + unit01B * (maxV - minV),
                    0.0f);
            }
            // Along: pick a segment of the CURVE in proportion to its length, then a point
            // along it, then an offset across it. Picking segments uniformly would crowd
            // the short ones and thin out the long ones, which on a coastline is backwards.
            const std::size_t spans = zone.closed ? zone.curve.size() : zone.curve.size() - 1;
            const auto segmentAt = [&](std::size_t index, Math::float3& a, Math::float3& b)
            {
                a = zone.curve[index];
                b = zone.curve[(index + 1) % zone.curve.size()];
            };
            const auto spanLength = [&](std::size_t index)
            {
                Math::float3 a, b;
                segmentAt(index, a, b);
                const float du = Axis(b, u) - Axis(a, u);
                const float dv = Axis(b, v) - Axis(a, v);
                return std::sqrt(du * du + dv * dv);
            };
            float total = 0.0f;
            for (std::size_t index = 0; index < spans; ++index)
            {
                total += spanLength(index);
            }
            float wanted = unit01A * total;
            std::size_t segment = 0;
            float t = 0.0f;
            for (std::size_t index = 0; index < spans; ++index)
            {
                const float length = spanLength(index);
                segment = index;
                if (wanted <= length || index + 1 == spans)
                {
                    t = length > 1e-4f ? std::min(wanted / length, 1.0f) : 0.0f;
                    break;
                }
                wanted -= length;
            }
            Math::float3 a, b;
            segmentAt(segment, a, b);
            const float dirU = Axis(b, u) - Axis(a, u);
            const float dirV = Axis(b, v) - Axis(a, v);
            const float length = std::sqrt(dirU * dirU + dirV * dirV);
            const float nu = length > 1e-4f ? -dirV / length : 0.0f;
            const float nv = length > 1e-4f ? dirU / length : 0.0f;
            const float across = (unit01B * 2.0f - 1.0f) * zone.halfWidth;
            // The third axis follows the CURVE here, not the centre: a path that climbs
            // should scatter at the height it climbs to, and lerping a and b along the same
            // t is what "along the curve" means on that axis too.
            const int w = 3 - u - v;
            const float other = Axis(a, w) + (Axis(b, w) - Axis(a, w)) * t;
            float xyz[3] = {};
            xyz[u] = Axis(a, u) + dirU * t + nu * across;
            xyz[v] = Axis(a, v) + dirV * t + nv * across;
            xyz[w] = other;
            return Math::float3(xyz[0], xyz[1], xyz[2]);
        }

        float localX = 0.0f;
        float localZ = 0.0f;
        if (zone.shape == Shape::Circle)
        {
            // sqrt makes it uniform BY AREA. Without it everything bunches at the centre,
            // which reads as a clump rather than a scatter -- the same reasoning the disc
            // scatter already used, kept because it was right. Sampling the unit circle and
            // then stretching keeps that uniformity when the circle is an ellipse.
            const float r = std::sqrt(unit01A);
            const float theta = unit01B * 6.2831853f;
            localX = r * std::cos(theta) * zone.halfX;
            localZ = r * std::sin(theta) * zone.halfZ;
        }
        else
        {
            localX = (unit01A * 2.0f - 1.0f) * zone.halfX;
            localZ = (unit01B * 2.0f - 1.0f) * zone.halfZ;
        }
        const float yaw = zone.yawDeg * 0.017453293f;
        const float c = std::cos(yaw);
        const float s = std::sin(yaw);
        return Math::float3(zone.centre.x + localX * c - localZ * s, zone.centre.y,
            zone.centre.z + localX * s + localZ * c);
    }

    float BoundingRadius(const Zone& zone)
    {
        if (zone.shape == Shape::Spline)
        {
            // Far enough to reach every point plus the band around it. Used to size the
            // obstacle search, so erring large costs a little work and erring small would
            // let a spawn grow a palm out of a rock just outside the search.
            float furthest = 0.0f;
            for (const Math::float3& point : zone.curve)
            {
                const float dx = point.x - zone.centre.x;
                const float dz = point.z - zone.centre.z;
                furthest = std::max(furthest, std::sqrt(dx * dx + dz * dz));
            }
            // The band only widens an `along` zone; an `inside` one ends at the curve.
            return furthest + (zone.fill == Fill::Along ? zone.halfWidth : 0.0f);
        }
        return zone.shape == Shape::Circle
            ? std::max(zone.halfX, zone.halfZ)
            : std::sqrt(zone.halfX * zone.halfX + zone.halfZ * zone.halfZ);
    }

    std::vector<std::vector<Math::float3>> OutlineLoops(const Zone& zone, int segments)
    {
        // A band around a CLOSED curve is an annulus: an outer ring and an inner ring, two
        // separate loops. Returning it as one loop -- out along one edge, back along the
        // other -- draws a line straight across the shape where the two edges meet, which
        // is the gap that showed up on screen.
        if (zone.shape == Shape::Spline && zone.fill == Fill::Along && zone.closed &&
            zone.curve.size() >= 2)
        {
            int u = 0;
            int v = 2;
            PlaneAxes(zone.plane, u, v);
            const std::size_t count = zone.curve.size();
            std::vector<Math::float3> outer;
            std::vector<Math::float3> inner;
            outer.reserve(count);
            inner.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                const Math::float3& prev = zone.curve[(index + count - 1) % count];
                const Math::float3& next = zone.curve[(index + 1) % count];
                const float dirU = Axis(next, u) - Axis(prev, u);
                const float dirV = Axis(next, v) - Axis(prev, v);
                const float length = std::sqrt(dirU * dirU + dirV * dirV);
                const float nu = length > 1e-4f ? -dirV / length : 0.0f;
                const float nv = length > 1e-4f ? dirU / length : 0.0f;
                const auto offset = [&](float sign)
                {
                    float xyz[3] = { zone.curve[index].x, zone.curve[index].y,
                        zone.curve[index].z };
                    xyz[u] += nu * zone.halfWidth * sign;
                    xyz[v] += nv * zone.halfWidth * sign;
                    return Math::float3(xyz[0], xyz[1], xyz[2]);
                };
                outer.push_back(offset(1.0f));
                inner.push_back(offset(-1.0f));
            }
            return { std::move(outer), std::move(inner) };
        }
        return { OutlinePoints(zone, segments) };
    }

    std::vector<Math::float3> OutlinePoints(const Zone& zone, int segments)
    {
        std::vector<Math::float3> points;
        if (zone.shape == Shape::Spline)
        {
            // Filling INSIDE: the curve itself is the boundary, and drawing a band around
            // it as well would show a shape that is not the one being filled.
            if (zone.fill == Fill::Inside)
            {
                return zone.curve;
            }
            // Along: both edges of the band, out one side and back the other, offset from
            // the TESSELLATED curve rather than from the control points -- offsetting the
            // control points is what made the first version look like a chain of rectangles.
            // The normal at each sample averages the segments either side of it, so the band
            // turns corners instead of stepping round them.
            const std::size_t count = zone.curve.size();
            points.reserve(count * 2);
            const auto normalAt = [&](std::size_t index, float& nx, float& nz)
            {
                const std::size_t prev = index == 0
                    ? (zone.closed ? count - 1 : 0) : index - 1;
                const std::size_t next = index + 1 >= count
                    ? (zone.closed ? 0 : count - 1) : index + 1;
                const float dirX = zone.curve[next].x - zone.curve[prev].x;
                const float dirZ = zone.curve[next].z - zone.curve[prev].z;
                const float length = std::sqrt(dirX * dirX + dirZ * dirZ);
                nx = length > 1e-4f ? -dirZ / length : 0.0f;
                nz = length > 1e-4f ? dirX / length : 0.0f;
            };
            for (std::size_t index = 0; index < count; ++index)
            {
                float nx = 0.0f;
                float nz = 0.0f;
                normalAt(index, nx, nz);
                points.push_back(Math::float3(zone.curve[index].x + nx * zone.halfWidth,
                    zone.curve[index].y, zone.curve[index].z + nz * zone.halfWidth));
            }
            for (std::size_t index = count; index-- > 0;)
            {
                float nx = 0.0f;
                float nz = 0.0f;
                normalAt(index, nx, nz);
                points.push_back(Math::float3(zone.curve[index].x - nx * zone.halfWidth,
                    zone.curve[index].y, zone.curve[index].z - nz * zone.halfWidth));
            }
            return points;
        }
        if (zone.shape == Shape::Circle)
        {
            const int steps = std::max(8, segments);
            const float yawC = zone.yawDeg * 0.017453293f;
            const float cc = std::cos(yawC);
            const float ss = std::sin(yawC);
            points.reserve(static_cast<std::size_t>(steps));
            for (int index = 0; index < steps; ++index)
            {
                const float theta = 6.2831853f * static_cast<float>(index) /
                    static_cast<float>(steps);
                // Rotated too: an ellipse that ignored yaw would be drawn at one angle and
                // tested at another, and only a diagonal drag would show it.
                const float lx = std::cos(theta) * zone.halfX;
                const float lz = std::sin(theta) * zone.halfZ;
                points.push_back(Math::float3(zone.centre.x + lx * cc - lz * ss,
                    zone.centre.y, zone.centre.z + lx * ss + lz * cc));
            }
            return points;
        }
        const float yaw = zone.yawDeg * 0.017453293f;
        const float c = std::cos(yaw);
        const float s = std::sin(yaw);
        const float corners[4][2] = {
            { -zone.halfX, -zone.halfZ }, {  zone.halfX, -zone.halfZ },
            {  zone.halfX,  zone.halfZ }, { -zone.halfX,  zone.halfZ },
        };
        points.reserve(4);
        for (const auto& corner : corners)
        {
            points.push_back(Math::float3(zone.centre.x + corner[0] * c - corner[1] * s,
                zone.centre.y, zone.centre.z + corner[0] * s + corner[1] * c));
        }
        return points;
    }

    int InsertPointAfter(EditorObject& object, int afterIndex)
    {
        std::vector<Math::float3> points = LocalPoints(object);
        if (points.size() < 2)
        {
            return -1;
        }
        const bool closed = object.properties.value("closed", false);
        const int count = static_cast<int>(points.size());
        // Clamped rather than refused: "after the last point" on an open spline means "at
        // the end", which is the commonest way anyone extends a path.
        int index = std::clamp(afterIndex, 0, count - 1);
        const bool wraps = closed && index == count - 1;
        if (!closed && index == count - 1)
        {
            // Extending past the end: carry on in the direction the last span was going,
            // rather than dropping a point on top of the last one.
            const Math::float3& a = points[static_cast<std::size_t>(count - 2)];
            const Math::float3& b = points[static_cast<std::size_t>(count - 1)];
            points.push_back(Math::float3(b.x + (b.x - a.x) * 0.5f,
                b.y + (b.y - a.y) * 0.5f, b.z + (b.z - a.z) * 0.5f));
            // STORE IT. Without this the vector grew, the index came back, and nothing was
            // written -- so "+" on the last point of an open spline did nothing at all,
            // which is every "+" anyone presses when extending a path.
            StoreLocalPoints(object, points);
            return count;
        }
        const Math::float3& a = points[static_cast<std::size_t>(index)];
        const Math::float3& b = points[static_cast<std::size_t>(wraps ? 0 : index + 1)];
        points.insert(points.begin() + index + 1,
            Math::float3((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f));
        StoreLocalPoints(object, points);
        return index + 1;
    }

    bool RemovePoint(EditorObject& object, int index)
    {
        std::vector<Math::float3> points = LocalPoints(object);
        // Two is the floor: one point is not a line, and everything here divides by a span.
        if (points.size() <= 2 || index < 0 || index >= static_cast<int>(points.size()))
        {
            return false;
        }
        points.erase(points.begin() + index);
        StoreLocalPoints(object, points);
        return true;
    }

    int NearestPoint(const Zone& zone, const Math::float3& position, float& outDistance)
    {
        int best = -1;
        float bestSq = 0.0f;
        for (std::size_t index = 0; index < zone.points.size(); ++index)
        {
            const float dx = zone.points[index].x - position.x;
            const float dz = zone.points[index].z - position.z;
            const float distanceSq = dx * dx + dz * dz;
            if (best < 0 || distanceSq < bestSq)
            {
                best = static_cast<int>(index);
                bestSq = distanceSq;
            }
        }
        outDistance = best < 0 ? 0.0f : std::sqrt(bestSq);
        return best;
    }

    int NearestSpan(const Zone& zone, const Math::float3& position)
    {
        const int count = static_cast<int>(zone.points.size());
        if (count < 2)
        {
            return -1;
        }
        const int spans = zone.closed ? count : count - 1;
        int best = -1;
        float bestSq = 0.0f;
        for (int span = 0; span < spans; ++span)
        {
            const Math::float3& a = zone.points[static_cast<std::size_t>(span)];
            const Math::float3& b = zone.points[static_cast<std::size_t>((span + 1) % count)];
            const float distanceSq = DistanceSqToSegment(position.x, position.z, a, b);
            if (best < 0 || distanceSq < bestSq)
            {
                best = span;
                bestSq = distanceSq;
            }
        }
        return best;
    }

    EditorObject BuildObject(Shape shape, const Math::float3& centre, float size,
        const std::string& name)
    {
        EditorObject object;
        object.name = name;
        object.type = kTypeName;
        object.enabled = true;
        object.transform.position = centre;
        object.transform.rotationDeg = Math::float3(0.0f, 0.0f, 0.0f);
        object.transform.scale = Math::float3(1.0f, 1.0f, 1.0f);
        object.properties = nlohmann::json::object();
        object.properties["shape"] = shape == Shape::Rect ? "rect"
            : shape == Shape::Spline ? "spline" : "circle";
        if (shape == Shape::Rect)
        {
            object.properties["halfX"] = size;
            object.properties["halfZ"] = size;
        }
        else if (shape == Shape::Spline)
        {
            // Three points in a shallow arc, not a straight line: a new spline should look
            // like something to bend rather than like a rectangle that lost its width, and
            // the middle handle is the one that teaches what dragging a point does.
            object.properties["halfWidth"] = size * 0.25f;
            object.properties["closed"] = false;
            object.properties["fill"] = "along";
            // Four points, not three: three in an arc can be mistaken for a fixed shape,
            // while four make it obvious the curve is made of movable points -- and a
            // closed four-point spline is already a usable blob.
            StoreLocalPoints(object, {
                Math::float3(-size, 0.0f, -size * 0.4f),
                Math::float3(-size * 0.35f, 0.0f, size * 0.45f),
                Math::float3(size * 0.35f, 0.0f, size * 0.45f),
                Math::float3(size, 0.0f, -size * 0.4f),
            });
        }
        else
        {
            object.properties["radius"] = size;
        }
        return object;
    }
}

#endif // WITH_EDITOR
