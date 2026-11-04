#!/usr/bin/env python3
r"""Fits an asset's collision: for every mesh asset, the cheapest shape that stays on its surface.

    python tools/collision_fit.py                  the table, nothing written
    python tools/collision_fit.py --write          also writes each verdict into its mesh.json "collision" block
    python tools/collision_fit.py --only <text>    only assets whose path holds <text>

Candidates, cheapest first: a sphere, a cylinder, a box, chains of cylinders along each connected part's long axis, a box per connected part (a frame,
a shelf), a convex hull. Each is measured both ways over sampled surfaces, in metres:

    onto   how far the mesh's surface lies from the shape's (the p95): a bump the shape cuts off, a part it does not hold
    away   how far the shape's surface lies from the mesh's (the p95): air that would stop a body, a corner with nothing in it

A shape passes when both stay under the asset's tolerance: 15 % of its middle size, between 1 and 4 cm; a sphere at half that (it rolls: only what is round).
Nothing passes, and 1 m or more (under it the rule's hull stands): the nearest of box and hull within the relaxed limit (a box twice the tolerance, a hull 30 % of the
middle size up to 15 cm: "at worst").
Still nothing = no block: the engine's rule (ShapeKind::Auto) stays, a hull under 1 m, the triangles above it.
PICKED names what the owner asked for by name: that kind, whatever else would pass. A palm's fit takes its trunk alone (slot 0): its fronds never collide,
and the crown's small wood goes with them, as on LOD 2.
What it writes is `"collision": {"shape": "custom", "shapes": [...]}` in the mesh's model space, in the Euler order of a level object's rotationDeg
(docs/physics_plan.md section 9.6); the Mesh Editor's Collision tab edits it from there. A block already there is left alone.
"""
import argparse
import collections
import glob
import json
import math
import os
import struct
import sys

import numpy as np
from scipy.spatial import ConvexHull, cKDTree

VERTEX_SIZE = 52
SUBMESH_SIZE = 12
MAX_CHAIN_PER_PART = 6
MAX_PARTS = 24
FLAT_THICKNESS = 0.05  # a sheet (a floor, a ceiling) becomes a box this thick behind its faces
# What the owner asked for by name (2026-11-04): the kind, and the slots it takes.
# (kind, slots, how far a chain's spine may stray from its slices, cylinders a part)
PICKED = {"coconut": ("sphere", None, None, None), "date_palm": ("chains", [0], 0.02, 6), "coconut_palm": ("chains", [0], 0.02, 6), "curly_palm": ("chains", [0], 0.02, 6), "young_palm": ("chains", [0], 0.02, 6)}
PICKED_PREFIX = {"lowpoly_sticks_node_stick_": ("chains", None, 0.012, 2)}
rng = np.random.default_rng(7)


def read_mesh(path):
    data = open(path, "rb").read()
    _magic, _version, _src, _opt, vertex_count, _lods = struct.unpack_from("<IIQQII", data, 0)
    off = 32
    floats = np.frombuffer(data, dtype=np.float32, count=vertex_count * VERTEX_SIZE // 4, offset=off)
    positions = floats.reshape(-1, VERTEX_SIZE // 4)[:, :3].astype(np.float64)
    off += vertex_count * VERTEX_SIZE
    index_count, submesh_count, _error = struct.unpack_from("<IIf", data, off)
    off += 12
    indices = np.frombuffer(data, dtype=np.uint32, count=index_count, offset=off).copy()
    off += index_count * 4
    submeshes = [struct.unpack_from("<III", data, off + i * SUBMESH_SIZE) for i in range(submesh_count)]
    return positions, indices, submeshes


def sample_surface(triangles, count):
    edge0 = triangles[:, 1] - triangles[:, 0]
    edge1 = triangles[:, 2] - triangles[:, 0]
    areas = 0.5 * np.linalg.norm(np.cross(edge0, edge1), axis=1)
    total = areas.sum()
    if total <= 0.0:
        return triangles.reshape(-1, 3)
    chosen = rng.choice(len(triangles), size=count, p=areas / total)
    u, v = rng.random(count), rng.random(count)
    flip = u + v > 1.0
    u[flip], v[flip] = 1.0 - u[flip], 1.0 - v[flip]
    return triangles[chosen, 0] + edge0[chosen] * u[:, None] + edge1[chosen] * v[:, None]


def surface_area(triangles):
    return float(0.5 * np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0]), axis=1).sum())


def connected_parts(positions, indices):
    """Index lists of the connected parts, vertices welded by position at 0.1 mm; the largest first."""
    _, weld = np.unique(np.round(positions * 10000.0).astype(np.int64), axis=0, return_inverse=True)
    weld = weld.ravel()
    parent = list(range(int(weld.max()) + 1))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for t in range(0, len(indices), 3):
        a = find(int(weld[indices[t]]))
        parent[find(int(weld[indices[t + 1]]))] = a
        parent[find(int(weld[indices[t + 2]]))] = a
    groups = collections.defaultdict(list)
    for t in range(0, len(indices), 3):
        groups[find(int(weld[indices[t]]))].append(t)
    parts = [indices[np.concatenate([np.arange(t, t + 3) for t in part])] for part in groups.values()]
    parts.sort(key=len, reverse=True)
    return parts


def principal_axes(points):
    """Rows: unit axes, the longest first, right-handed."""
    centred = points - points.mean(axis=0)
    _values, vectors = np.linalg.eigh(np.cov(centred.T))
    axes = vectors[:, ::-1].T
    if np.dot(np.cross(axes[0], axes[1]), axes[2]) < 0.0:
        axes[2] = -axes[2]
    return axes


def euler_from_rows(rows):
    """Math.h EulerXYZRadFromRotation: rows are the images of the local x, y, z (DirectX row vectors), degrees."""
    s = -rows[0, 2]
    c = math.hypot(rows[0, 0], rows[0, 1])
    y = math.atan2(s, c)
    if c < 1e-5:
        x = math.atan2(rows[1, 0] if s > 0.0 else -rows[1, 0], rows[1, 1])
        z = 0.0
    else:
        x = math.atan2(rows[1, 2], rows[2, 2])
        z = math.atan2(rows[0, 1], rows[0, 0])
    return [math.degrees(x), math.degrees(y), math.degrees(z)]


def rows_with_y(direction):
    """A rotation whose local y is this direction."""
    y = direction / np.linalg.norm(direction)
    helper = np.array([1.0, 0.0, 0.0]) if abs(y[0]) < 0.9 else np.array([0.0, 0.0, 1.0])
    z = np.cross(helper, y)
    z /= np.linalg.norm(z)
    x = np.cross(y, z)
    return np.array([x, y, z])


# A primitive: signed distance of points to it (negative inside), samples on its surface at this spacing.
class Sphere:
    def __init__(self, centre, radius):
        self.centre, self.radius = centre, radius

    def signed(self, points):
        return np.linalg.norm(points - self.centre, axis=1) - self.radius

    def samples(self, spacing):
        n = int(np.clip(4.0 * math.pi * self.radius ** 2 / spacing ** 2, 64, 60000))
        d = rng.normal(size=(n, 3))
        return self.centre + self.radius * d / np.linalg.norm(d, axis=1, keepdims=True)

    def as_dict(self):
        return {"type": "sphere", "center": self.centre.tolist(), "radius": self.radius}


class Cylinder:
    def __init__(self, centre, axis, half_height, radius):
        self.centre, self.axis, self.half_height, self.radius = centre, axis, half_height, radius

    def signed(self, points):
        rel = points - self.centre
        along = rel @ self.axis
        across = np.linalg.norm(rel - np.outer(along, self.axis), axis=1)
        dr = across - self.radius
        dh = np.abs(along) - self.half_height
        return np.hypot(np.maximum(dr, 0.0), np.maximum(dh, 0.0)) + np.minimum(np.maximum(dr, dh), 0.0)

    def samples(self, spacing):
        rows = rows_with_y(self.axis)
        n_side = int(np.clip(2.0 * math.pi * self.radius * 2.0 * self.half_height / spacing ** 2, 32, 60000))
        angle = rng.random(n_side) * 2.0 * math.pi
        h = (rng.random(n_side) * 2.0 - 1.0) * self.half_height
        side = self.centre + np.outer(np.cos(angle) * self.radius, rows[0]) + np.outer(h, rows[1]) + np.outer(np.sin(angle) * self.radius, rows[2])
        n_cap = int(np.clip(2.0 * math.pi * self.radius ** 2 / spacing ** 2, 16, 60000))
        angle = rng.random(n_cap) * 2.0 * math.pi
        r = np.sqrt(rng.random(n_cap)) * self.radius
        sign = np.where(rng.random(n_cap) < 0.5, -1.0, 1.0)
        caps = self.centre + np.outer(np.cos(angle) * r, rows[0]) + np.outer(sign * self.half_height, rows[1]) + np.outer(np.sin(angle) * r, rows[2])
        return np.vstack([side, caps])

    def as_dict(self):
        return {"type": "cylinder", "center": self.centre.tolist(), "rotationDeg": euler_from_rows(rows_with_y(self.axis)), "radius": self.radius, "halfHeight": self.half_height}


class Capsule:
    def __init__(self, centre, axis, half_height, radius):
        self.centre, self.axis, self.half_height, self.radius = centre, axis, half_height, radius

    def signed(self, points):
        rel = points - self.centre
        along = np.clip(rel @ self.axis, -self.half_height, self.half_height)
        return np.linalg.norm(rel - np.outer(along, self.axis), axis=1) - self.radius

    def samples(self, spacing):
        area = 4.0 * math.pi * self.radius ** 2 + 2.0 * math.pi * self.radius * 2.0 * self.half_height
        n = int(np.clip(area / spacing ** 2, 64, 60000))
        d = rng.normal(size=(n, 3))
        d /= np.linalg.norm(d, axis=1, keepdims=True)
        along = (rng.random(n) * 2.0 - 1.0) * self.half_height
        return self.centre + d * self.radius + np.outer(along, self.axis)

    def as_dict(self):
        return {"type": "capsule", "center": self.centre.tolist(), "rotationDeg": euler_from_rows(rows_with_y(self.axis)), "radius": self.radius, "halfHeight": self.half_height}


def fit_capsule(mesh):
    """Along the long axis, through the box's centre in that frame; the radius and the height that keep the worse of the two ways smallest, the sum settling a tie."""
    rows = principal_axes(mesh.points)
    local = mesh.points @ rows.T
    centre = (0.5 * (local.min(axis=0) + local.max(axis=0))) @ rows
    rel = mesh.points - centre
    along = rel @ rows[0]
    across = np.linalg.norm(rel - np.outer(along, rows[0]), axis=1)
    reach = 0.5 * float(along.max() - along.min())
    best = None
    for radius in np.linspace(np.percentile(across, 30), np.percentile(across, 99), 12):
        for share in (0.55, 0.65, 0.75, 0.85, 0.95, 1.0):
            capsule = Capsule(centre, rows[0], max(share * reach - radius, 0.001), float(radius))
            onto, away = measure(mesh, [capsule])
            score = max(onto, away) + 0.1 * (onto + away)
            if best is None or score < best[0]:
                best = (score, capsule)
    return [best[1]]


class Box:
    def __init__(self, centre, rows, half):
        self.centre, self.rows, self.half = centre, rows, half

    def signed(self, points):
        q = np.abs((points - self.centre) @ self.rows.T) - self.half
        return np.linalg.norm(np.maximum(q, 0.0), axis=1) + np.minimum(q.max(axis=1), 0.0)

    def samples(self, spacing):
        e = 2.0 * self.half
        areas = np.array([e[1] * e[2], e[0] * e[2], e[0] * e[1]])
        n = int(np.clip(2.0 * areas.sum() / spacing ** 2, 64, 120000))
        picks = rng.choice(3, size=n, p=areas / max(areas.sum(), 1e-12))
        s = (rng.random((n, 3)) * 2.0 - 1.0) * self.half
        s[np.arange(n), picks] = np.where(rng.random(n) < 0.5, -1.0, 1.0) * self.half[picks]
        return self.centre + s @ self.rows

    def as_dict(self):
        shape = {"type": "box", "center": self.centre.tolist(), "halfExtents": self.half.tolist()}
        euler = euler_from_rows(self.rows)
        if max(abs(v) for v in euler) > 1e-3:
            shape["rotationDeg"] = euler
        return shape


class Mesh:
    """The asset's surface, sampled dense enough that the nearest sample's distance is the surface's."""

    def __init__(self, positions, indices, tolerance):
        self.triangles = positions[indices].reshape(-1, 3, 3)
        self.area = surface_area(self.triangles)
        self.spacing = max(tolerance / 3.0, math.sqrt(self.area / 400000.0))
        dense = sample_surface(self.triangles, int(np.clip(self.area / self.spacing ** 2, 2000, 400000)))
        self.tree = cKDTree(dense)
        self.points = dense[rng.choice(len(dense), size=min(len(dense), 20000), replace=False)]


def measure(mesh, primitives):
    """p95 both ways. A primitive's surface inside another primitive is no surface of the shape: a chain's inner caps, a frame's overlaps."""
    distances = np.stack([np.abs(p.signed(mesh.points)) for p in primitives])
    onto = distances.min(axis=0)
    # A mesh point inside a primitive but far from its faces is still what the shape misses: the absolute distance is the right one above.
    samples = []
    for i, primitive in enumerate(primitives):
        own = primitive.samples(mesh.spacing)
        for j, other in enumerate(primitives):
            if j != i and len(own) > 0:
                own = own[other.signed(own) > -1e-4]
        if len(own) > 0:
            samples.append(own)
    away, _ = mesh.tree.query(np.vstack(samples))
    return float(np.percentile(onto, 95)), float(np.percentile(away, 95))


def fit_sphere(mesh):
    centre = 0.5 * (mesh.points.min(axis=0) + mesh.points.max(axis=0))
    return [Sphere(centre, float(np.median(np.linalg.norm(mesh.points - centre, axis=1))))]


def cylinder_between(points, a, b):
    """The cylinder from a to b; its radius the p75 distance from the axis of the points it holds (a trunk's bark stands proud)."""
    axis = b - a
    length = float(np.linalg.norm(axis))
    axis = axis / max(length, 1e-9)
    centre = 0.5 * (a + b)
    rel = points - centre
    along = rel @ axis
    held = np.abs(along) <= 0.5 * length
    if held.sum() < 4:
        held = np.ones(len(points), dtype=bool)
    across = np.linalg.norm(rel[held] - np.outer(along[held], axis), axis=1)
    return Cylinder(centre, axis, 0.5 * length, max(float(np.percentile(across, 75)), 0.001))


def fit_chain(points, tolerance, max_segments):
    """Cylinders along a part's long axis: the slices' centres and radii, the polyline simplified until it stays within the tolerance."""
    axes = principal_axes(points)
    origin = points.mean(axis=0)
    t = (points - origin) @ axes[0]
    lo, hi = float(t.min()), float(t.max())
    if hi - lo < 1e-4:
        return None
    slices = 24
    spine = []
    for s in range(slices):
        a0 = lo + (hi - lo) * s / slices
        a1 = lo + (hi - lo) * (s + 1) / slices
        held = points[(t >= a0) & (t <= a1)]
        if len(held) < 4:
            continue
        centre = 0.5 * (0.5 * (held.min(axis=0) + held.max(axis=0)) + held.mean(axis=0))
        across = np.linalg.norm((held - centre) - np.outer((held - centre) @ axes[0], axes[0]), axis=1)
        spine.append(np.append(centre, float(np.percentile(across, 75))))
    if len(spine) < 2:
        return None
    spine = np.array(spine)
    spine[0, :3] += axes[0] * (lo - (spine[0, :3] - origin) @ axes[0])  # the ends: out to the part's extremes
    spine[-1, :3] += axes[0] * (hi - (spine[-1, :3] - origin) @ axes[0])

    def simplify(first, last, keep):
        if last <= first + 1:
            return
        a, b = spine[first], spine[last]
        ab = b[:3] - a[:3]
        length = max(float(np.dot(ab, ab)), 1e-12)
        worst, at = 0.0, -1
        for i in range(first + 1, last):
            k = float(np.dot(spine[i, :3] - a[:3], ab)) / length
            off = np.linalg.norm(spine[i, :3] - (a[:3] + ab * k))
            error = max(off, abs(spine[i, 3] - (a[3] + (b[3] - a[3]) * k)))
            if error > worst:
                worst, at = error, i
        if worst > tolerance:
            keep.add(at)
            simplify(first, at, keep)
            simplify(at, last, keep)

    keep = {0, len(spine) - 1}
    simplify(0, len(spine) - 1, keep)
    knots = sorted(keep)
    while len(knots) - 1 > max_segments:  # the straightest knot goes
        errors = []
        for k in range(1, len(knots) - 1):
            a, b, c = spine[knots[k - 1], :3], spine[knots[k], :3], spine[knots[k + 1], :3]
            ab = c - a
            errors.append(np.linalg.norm((b - a) - ab * (np.dot(b - a, ab) / max(np.dot(ab, ab), 1e-12))))
        knots.pop(1 + int(np.argmin(errors)))
    return [cylinder_between(points, spine[knots[k], :3], spine[knots[k + 1], :3]) for k in range(len(knots) - 1)]


def fit_chains(positions, indices, tolerance, max_segments):
    """A chain per connected part, the parts a tenth of the longest or more: a palm with two trunks is two chains."""
    pieces = connected_parts(positions, indices)
    if len(pieces) > 40:
        return None
    extents = [float(np.max(positions[p].max(axis=0) - positions[p].min(axis=0))) for p in pieces]
    shapes = []
    for piece, extent in zip(pieces, extents):
        if extent < 0.1 * max(extents):
            continue
        triangles = positions[piece].reshape(-1, 3, 3)
        points = sample_surface(triangles, 4000)
        chain = fit_chain(points, tolerance, max_segments)
        if chain:
            shapes.extend(chain)
    return shapes or None


def box_of(points, trim):
    """The tighter of the mesh's own axes and its principal ones; trim = the share of surface each face may leave outside (a skirting board on a wall)."""
    best = None
    for rows in (np.eye(3), principal_axes(points)):
        local = points @ rows.T
        lo, hi = np.percentile(local, trim, axis=0), np.percentile(local, 100.0 - trim, axis=0)
        volume = float(np.prod(np.maximum(hi - lo, 1e-4)))
        if best is None or volume < best[0] * 0.97:  # the mesh's own axes unless the turn pays
            best = (volume, rows, lo, hi)
    _, rows, lo, hi = best
    return Box((0.5 * (lo + hi)) @ rows, rows, np.maximum(0.5 * (hi - lo), 0.001))


def fit_boxes_by_part(positions, indices):
    pieces = connected_parts(positions, indices)
    if len(pieces) < 2:
        return None
    # Tiny pieces (a screw, a rivet) ride on what is around them: a body cannot tell.
    size = float(np.max(positions[indices].max(axis=0) - positions[indices].min(axis=0)))
    kept = [p for p in pieces if float(np.max(positions[p].max(axis=0) - positions[p].min(axis=0))) >= 0.03 * size]
    if not 2 <= len(kept) <= MAX_PARTS:
        return None
    return [box_of(sample_surface(positions[p].reshape(-1, 3, 3), 800), 0.0) for p in kept]


def thicken_sheet(box, triangles):
    """A sheet has no inside: the box goes on behind its faces, so a floor stops what lands on it."""
    if box.half.min() >= 0.5 * FLAT_THICKNESS:
        return box
    axis = int(np.argmin(box.half))
    normal = box.rows[axis]
    faces = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0]).sum(axis=0)
    behind = -1.0 if np.dot(faces, normal) > 0.0 else 1.0  # clockwise from the front in a left-handed space: the cross product points to the front
    half = box.half.copy()
    centre = box.centre + normal * behind * (0.5 * FLAT_THICKNESS - box.half[axis])
    half[axis] = 0.5 * FLAT_THICKNESS
    return Box(centre, box.rows, half)


def verdict_for(positions, indices, picked, want_capsule=False):
    points = positions[indices]
    size = np.sort(points.max(axis=0) - points.min(axis=0))
    tolerance = float(np.clip(0.15 * size[1], 0.01, 0.04))
    mesh = Mesh(positions, indices, tolerance)
    results = []

    def consider(label, primitives, limit):
        if primitives:
            onto, away = measure(mesh, primitives)
            results.append({"label": label, "primitives": primitives, "onto": onto, "away": away, "pass": onto <= limit and away <= limit})

    consider("sphere", fit_sphere(mesh), 0.5 * tolerance)
    if want_capsule:
        consider("capsule", fit_capsule(mesh), 0.0)  # measured, never chosen unless picked
    single = fit_chain(mesh.points, 1e9, 1)
    if single:
        consider("cylinder", single, tolerance)
    tight, trimmed = box_of(mesh.points, 0.0), box_of(mesh.points, 3.0)
    measured = sorted(((max(measure(mesh, [b])), i, b) for i, b in enumerate((tight, trimmed))))
    consider("box", [measured[0][2]], tolerance)
    if picked and picked[0] == "chains":
        chains = fit_chains(positions, indices, picked[2], picked[3])
    else:
        # A chain earns its place where nothing convex holds the shape: above the 1 m a hull is not the rule, in three cylinders at most.
        chains = fit_chains(positions, indices, 0.5 * tolerance, 3) if size[2] >= 1.0 else None
        chains = chains if chains and len(chains) <= 3 else None
    if chains and len(chains) > 1:
        consider("chains %d" % len(chains), chains, tolerance)
    boxes = fit_boxes_by_part(positions, indices)
    if boxes:
        consider("boxes %d" % len(boxes), boxes, tolerance)
    hull = None
    try:
        vertices = positions[np.unique(indices)]
        hull = ConvexHull(vertices)
    except Exception:
        pass
    if hull is not None:
        planes = hull.equations
        onto = np.abs((mesh.points @ planes[:, :3].T + planes[:, 3]).max(axis=1))
        away, _ = mesh.tree.query(sample_surface(vertices[hull.simplices], 20000))
        o, a = float(np.percentile(onto, 95)), float(np.percentile(away, 95))
        results.append({"label": "hull", "primitives": None, "onto": o, "away": a, "pass": o <= tolerance and a <= tolerance})
    return results, tolerance, size, mesh


def collision_block(result, mesh):
    if result["label"] == "hull":
        return {"shape": "hull"}

    def rounded(value):
        if isinstance(value, list):
            return [rounded(v) for v in value]
        return round(value, 4) if isinstance(value, float) else value

    primitives = [thicken_sheet(p, mesh.triangles) if isinstance(p, Box) else p for p in result["primitives"]]
    return {"shape": "custom", "shapes": [{k: rounded(v) for k, v in p.as_dict().items()} for p in primitives]}


def write_block(path, original, block):
    """Inserted as text where its sorted key belongs: no other line of the file moves."""
    newline = "\r\n" if "\r\n" in original else "\n"
    lines = original.replace("\r\n", "\n").split("\n")
    body = json.dumps({"collision": block}, indent=2, ensure_ascii=False, sort_keys=True).split("\n")[1:-1]
    top = [i for i, line in enumerate(lines) if line.startswith('  "') and '":' in line]
    keys = [lines[i][3:lines[i].index('":')] for i in top]
    after = next((i for i, key in zip(top, keys) if key > "collision"), None)
    if after is not None:
        body[-1] += ","
        lines[after:after] = body
    else:
        last = next(i for i in range(len(lines) - 1, -1, -1) if lines[i].rstrip() == "}") - 1
        lines[last] += ","
        lines[last + 1:last + 1] = body
    updated = "\n".join(lines)
    json.loads(updated)  # still JSON, or this raises before anything is written
    with open(path, "wb") as f:
        f.write(updated.replace("\n", newline).encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true", help="write each verdict into its mesh.json")
    parser.add_argument("--only", default="", help="only assets whose path holds this")
    parser.add_argument("--capsule", action="store_true", help="also fit the best capsule, and print its block")
    args = parser.parse_args()
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    placed = collections.Counter()
    for level_path in glob.glob(os.path.join(root, "data", "levels", "*.json")):
        try:
            level = json.load(open(level_path, encoding="utf-8"))
        except Exception:
            continue
        for item in level.get("objects", []):
            if item.get("mesh"):
                placed[item["mesh"].replace("\\", "/")] += 1
    tally = collections.Counter()
    rows = []
    for path in sorted(glob.glob(os.path.join(root, "models", "**", "*.mesh.json"), recursive=True)):
        rel = os.path.relpath(path, root).replace("\\", "/")
        if args.only and args.only not in rel:
            continue
        raw = open(path, "rb").read()
        manifest = json.loads(raw.decode("utf-8"))
        geometry = manifest.get("geometry", "")
        if not geometry.endswith(".mesh.bin") or not os.path.exists(os.path.join(root, geometry)):
            continue
        positions, indices, submeshes = read_mesh(os.path.join(root, geometry))
        foliage = manifest.get("windFoliage") or []
        stem = os.path.basename(rel).replace(".mesh.json", "")
        picked = PICKED.get(stem) or next((v for k, v in PICKED_PREFIX.items() if stem.startswith(k)), None)
        slots = picked[1] if picked else None
        lists = [indices[first:first + count] for first, count, slot in submeshes if count > 0
                 and (slot in slots if slots is not None else not (slot < len(foliage) and foliage[slot] > 0))]
        if not lists:
            tally["only leaves: no body"] += 1
            continue
        solid = np.concatenate(lists)
        extent = positions[solid].max(axis=0) - positions[solid].min(axis=0)
        results, tolerance, size, mesh = verdict_for(positions, solid, picked, args.capsule)
        if args.capsule:
            capsule = next((r for r in results if r["label"] == "capsule"), None)
            print("capsule block of", rel, json.dumps(collision_block(capsule, mesh)))
        chosen = None
        if picked:
            kind = picked[0]
            chosen = next((r for r in results if r["label"].split(" ")[0] == kind), None)
            if kind == "chains" and chosen is None:
                chosen = next((r for r in results if r["label"] == "cylinder"), None)
        else:
            chosen = next((r for r in results if r["pass"]), None)
            if chosen is None and max(extent) >= 1.0:
                relaxed = [r for r in results if r["label"] in ("box", "hull") and max(r["onto"], r["away"]) <= (2.0 * tolerance if r["label"] == "box" else float(np.clip(0.3 * size[1], 0.02, 0.15)))]
                chosen = min(relaxed, key=lambda r: max(r["onto"], r["away"])) if relaxed else None
        small = max(extent) < 1.0
        if chosen and chosen["label"] == "hull" and small:
            chosen = None  # the rule already gives it a hull: nothing to write
        label = chosen["label"] if chosen else ("hull (rule)" if small else "mesh (rule)")
        tally[label.split(" ")[0] + (" (rule)" if "(rule)" in label else "")] += 1
        numbers = " ".join("%s %.1f/%.1f%s" % (r["label"], r["onto"] * 100.0, r["away"] * 100.0, "*" if r is chosen else "") for r in results)
        rows.append((placed.get(rel, 0), rel, len(solid) // 3, size, tolerance, label, numbers))
        if args.write and chosen and "collision" not in manifest:
            write_block(path, raw.decode("utf-8"), collision_block(chosen, mesh))
    rows.sort(key=lambda r: (-r[0], r[1]))
    print("placed tris   size (m)              tol   verdict      candidates: onto/away p95 in cm, * = chosen")
    for n, rel, tris, size, tolerance, label, numbers in rows:
        print("%5d %6d  %6.2f %6.2f %6.2f  %4.1f  %-11s %s  %s" % (n, tris, size[2], size[1], size[0], tolerance * 100.0, label, rel.replace("models/", "").replace(".mesh.json", ""), numbers))
    print("verdicts:", dict(tally))
    print("triangles of the assets placed, by verdict:", {k: sum(r[2] for r in rows if r[0] and r[5].split(" ")[0] == k) for k in {r[5].split(" ")[0] for r in rows}})


if __name__ == "__main__":
    sys.exit(main())
