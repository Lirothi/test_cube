#pragma once
#if WITH_EDITOR

#include <string>
#include <vector>

#include "core/math/Math.h"
#include "editor/scene/EditorSceneDocument.h"

// A named region of the level, drawn on the ground, that a command can point at.
//
// It exists because there was no way to say WHERE. `spawn` scattered over a disc around
// whatever the camera happened to be looking at, and that disc was the only spatial idea in
// the whole vocabulary -- so "раскидай камни только на пляже, не в воде" was refused, and
// "поставь пальмы вдоль берега" was answered with a 15 m circle, which is worse than a
// refusal. A zone is that disc generalised and, crucially, given a NAME: it survives the
// camera moving, it can be pointed at twice, and the second command means the same as the
// first.
//
// A zone is an ordinary document object, not a new kind of entity, and that is most of why
// this is small. `freeCameraStart` set the precedent: a type with no renderable behind it.
// Being an object buys the name, the outliner row, selection, the translate gizmo, undo and
// level round-tripping without a line of new code for any of them.
namespace editorzone
{
    // The type string an object carries to be a zone. Nothing renders it, exactly as
    // nothing renders a freeCameraStart.
    inline constexpr const char* kTypeName = "zone";

    enum class Shape
    {
        Circle,
        Rect,
        // A band of constant width following a polyline. This is the shape "поставь пальмы
        // вдоль берега" wants and neither of the others can express: a coastline is not a
        // disc and not a box, and answering it with either is why that phrase used to come
        // back as a 15 m circle around the camera.
        Spline,
    };

    // What a SPLINE zone covers. A coastline and a meadow are both splines and they are not
    // the same region: one is a ribbon along the line, the other is everything the line
    // encloses.
    // Which two axes a spline is measured in. Points are full 3D, so a path can climb a
    // dune or run up a cliff face; but "inside the outline" and "within the band" are
    // two-dimensional questions, and which two depends on what the shape is FOR. A trail
    // over ground is XZ; a line of torches up a wall is XY.
    enum class Plane
    {
        XZ,   // ground plan, the default and the one terrain scattering wants
        XY,
        ZY,
    };

    enum class Fill
    {
        // A band of halfWidth either side of the curve. "поставь пальмы вдоль берега".
        Along,
        // Everything inside the curve. If the spline is not explicitly closed it is closed
        // for the purpose of this test -- an open line encloses nothing, and refusing the
        // request over a flag the user never set would be pedantry, not safety.
        Inside,
    };

    struct Zone
    {
        EditorObjectId id;
        std::string name;
        Shape shape = Shape::Circle;
        // Centre comes from the object's TRANSFORM, so dragging the gizmo moves the zone and
        // Ctrl+Z puts it back -- no zone-specific editing path, and no second source of
        // truth about where it is.
        Math::float3 centre{ 0.0f, 0.0f, 0.0f };
        float yawDeg = 0.0f;
        // Half-extents on X and Z, already multiplied by the object's scale -- which is
        // what makes the SCALE gizmo resize a zone instead of spinning uselessly. A circle
        // whose two are equal is a circle; dragging one axis makes it an ellipse, which is
        // the honest reading of the handle that was dragged and is a useful shape besides.
        float halfX = 25.0f;
        float halfZ = 25.0f;
        // Spline only. `points` are the CONTROL points in world space; `curve` is the
        // Catmull-Rom curve through them, tessellated, also in world space. Straight
        // segments between control points were the first attempt and they looked like what
        // they were -- a chain of rectangles, which is not a coastline.
        //
        // Every consumer reads `curve`, never `points`: containment, sampling and the
        // outline all want the actual shape, and only the editor cares which points made it.
        std::vector<Math::float3> points;
        std::vector<Math::float3> curve;
        float halfWidth = 8.0f;
        // A closed spline is a loop: the curve wraps from the last control point back to
        // the first, with no end to it.
        bool closed = false;
        Fill fill = Fill::Along;
        Plane plane = Plane::XZ;
    };

    // How big the draggable spheres at the control points are, in metres. A property of
    // the zone rather than a global: a 500 m coastline and a 4 m flower bed want very
    // different handles, and one number for both is wrong for one of them.
    float PointRadius(const EditorObject& object);

    // The two axes of a plane, as indices into a float3 (0=x, 1=y, 2=z).
    void PlaneAxes(Plane plane, int& outU, int& outV);
    // The whole outline as one or more CLOSED loops. A band around a closed curve is an
    // annulus -- two separate rings -- and drawing it as a single loop puts a seam across
    // the shape where the outer edge jumps to the inner one.
    std::vector<std::vector<Math::float3>> OutlineLoops(const Zone& zone, int segments = 48);

    // The spline's control points as stored: local to the object, before its transform.
    // The editor writes these back when a point is dragged, added or removed.
    std::vector<Math::float3> LocalPoints(const EditorObject& object);
    void StoreLocalPoints(EditorObject& object, const std::vector<Math::float3>& points);

    // Insert a control point after `afterIndex`, halfway along the curve to the next one.
    // Returns the index of the new point, or -1 when the object is not a spline.
    int InsertPointAfter(EditorObject& object, int afterIndex);
    // Remove one. Refuses below two points, which is the smallest thing still a line.
    bool RemovePoint(EditorObject& object, int index);
    // Which control point a world position is nearest, and how far in metres. -1 when the
    // object is not a spline or has no points.
    int NearestPoint(const Zone& zone, const Math::float3& position, float& outDistance);
    // Which SPAN of the curve a world position is nearest -- i.e. after which control point
    // a new one should go. -1 when there is no span.
    int NearestSpan(const Zone& zone, const Math::float3& position);

    // Reads a zone out of a document object, or returns false when the object is not one.
    bool FromObject(const EditorObject& object, Zone& outZone);

    // Every zone in the document, in document order.
    std::vector<Zone> Collect(const EditorSceneDocument& document);

    // By name, case-insensitively and by substring, matching how every other name in this
    // layer is resolved -- the user says "beach", the zone is called "Beach Zone".
    // Returns false when nothing matches, or when more than one does: an ambiguous WHERE
    // silently picking one is the failure mode this whole layer is built to avoid.
    bool Find(const EditorSceneDocument& document,
        const std::string& needle,
        Zone& outZone,
        std::string& outWhyNot);

    // Is this world position inside the zone? Y is ignored: a zone is a footprint on the
    // ground, and the things it governs are dropped onto that ground anyway.
    bool Contains(const Zone& zone, const Math::float3& position);

    // A point drawn uniformly by AREA from inside the zone. `unit01` supplies two
    // independent uniforms in [0,1) so the caller keeps its own RNG and its own seed.
    Math::float3 SamplePoint(const Zone& zone, float unit01A, float unit01B);

    // The radius of a circle that contains the whole zone, for sizing the obstacle search
    // and for reporting.
    float BoundingRadius(const Zone& zone);

    // The outline as a closed loop of world points, for drawing.
    std::vector<Math::float3> OutlinePoints(const Zone& zone, int segments = 48);

    // A fresh zone object, centred where asked, ready for CreateDocumentObjectCommand.
    EditorObject BuildObject(Shape shape, const Math::float3& centre, float size,
        const std::string& name);
}

#endif // WITH_EDITOR
