#!/usr/bin/env python3
"""Populate the atoll level with deterministic, terrain-conforming palms.

Existing palm entries are replaced, making repeated runs idempotent.  Candidate
positions are sampled by triangle area from the authored atoll mesh and rejected
when they are underwater, too steep, inside the inlet/beach sector, too close to
another tree, or too close to an existing prop.
"""

from __future__ import annotations

import argparse
import bisect
import json
import math
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


Vec3 = tuple[float, float, float]

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LEVEL = REPO_ROOT / "data" / "levels" / "atoll.json"
DEFAULT_MESH = REPO_ROOT / "models" / "atoll_island.obj"

PALM_MODELS = (
    "import_staging/coconut_palm/scene.gltf",
    "import_staging/date_palm/scene.gltf",
    "import_staging/curly_palm/scene.gltf",
)


@dataclass(frozen=True)
class SurfaceTriangle:
    a: Vec3
    b: Vec3
    c: Vec3
    area: float


def _sub(a: Vec3, b: Vec3) -> Vec3:
    return a[0] - b[0], a[1] - b[1], a[2] - b[2]


def _cross(a: Vec3, b: Vec3) -> Vec3:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _length(v: Vec3) -> float:
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def _read_surface_triangles(mesh_path: Path, min_normal_y: float) -> list[SurfaceTriangle]:
    vertices: list[Vec3] = []
    faces: list[tuple[int, int, int]] = []
    with mesh_path.open("r", encoding="utf-8") as mesh_file:
        for line in mesh_file:
            if line.startswith("v "):
                _, x, y, z = line.split()
                vertices.append((float(x), float(y), float(z)))
            elif line.startswith("f "):
                indices = [int(part.split("/", 1)[0]) - 1 for part in line.split()[1:]]
                if len(indices) != 3:
                    raise ValueError(f"Expected triangulated OBJ face, got: {line.rstrip()}")
                faces.append((indices[0], indices[1], indices[2]))

    triangles: list[SurfaceTriangle] = []
    for ia, ib, ic in faces:
        a, b, c = vertices[ia], vertices[ib], vertices[ic]
        cross = _cross(_sub(b, a), _sub(c, a))
        double_area = _length(cross)
        if double_area <= 1.0e-8:
            continue
        normal_y = abs(cross[1]) / double_area
        if normal_y < min_normal_y:
            continue
        triangles.append(SurfaceTriangle(a, b, c, 0.5 * double_area))

    if not triangles:
        raise ValueError(f"No usable upward-facing triangles found in {mesh_path}")
    return triangles


def _sample_triangle(triangle: SurfaceTriangle, rng: random.Random) -> Vec3:
    # Square-root barycentric sampling is uniform over triangle area.
    root = math.sqrt(rng.random())
    weight_a = 1.0 - root
    weight_b = root * (1.0 - rng.random())
    weight_c = 1.0 - weight_a - weight_b
    return (
        weight_a * triangle.a[0] + weight_b * triangle.b[0] + weight_c * triangle.c[0],
        weight_a * triangle.a[1] + weight_b * triangle.b[1] + weight_c * triangle.c[1],
        weight_a * triangle.a[2] + weight_b * triangle.b[2] + weight_c * triangle.c[2],
    )


def _angle_delta_degrees(a: float, b: float) -> float:
    return abs((a - b + 180.0) % 360.0 - 180.0)


def _xz_distance_squared(a: Sequence[float], b: Sequence[float]) -> float:
    dx = float(a[0]) - float(b[0])
    dz = float(a[2]) - float(b[2])
    return dx * dx + dz * dz


def _prop_positions(objects: Sequence[dict[str, Any]]) -> list[Vec3]:
    terrain_models = {"models/atoll_island.obj", "models/atoll_lagoon_floor.obj"}
    positions: list[Vec3] = []
    for obj in objects:
        if obj.get("type") != "staticMesh" or obj.get("model") in terrain_models:
            continue
        position = obj.get("position")
        if isinstance(position, list) and len(position) == 3:
            positions.append((float(position[0]), float(position[1]), float(position[2])))
    return positions


def _find_island_translation(objects: Sequence[dict[str, Any]], mesh_path: Path) -> Vec3:
    normalized_mesh = mesh_path.resolve()
    for obj in objects:
        model = obj.get("model")
        if not isinstance(model, str):
            continue
        model_path = (REPO_ROOT / model).resolve()
        if model_path != normalized_mesh:
            continue
        if obj.get("rotationDeg", [0.0, 0.0, 0.0]) != [0.0, 0.0, 0.0]:
            raise ValueError("Atoll palm population expects an unrotated island mesh")
        if obj.get("scale", [1.0, 1.0, 1.0]) != [1.0, 1.0, 1.0]:
            raise ValueError("Atoll palm population expects an unscaled island mesh")
        position = obj.get("position", [0.0, 0.0, 0.0])
        return float(position[0]), float(position[1]), float(position[2])
    raise ValueError(f"Level does not contain island mesh {mesh_path}")


def _make_palm_object(
    object_id: int,
    index: int,
    model: str,
    position: Vec3,
    yaw_degrees: float,
    scale: float,
) -> dict[str, Any]:
    variant = Path(model).parent.name.replace("_", " ").title()
    return {
        "enabled": True,
        "id": object_id,
        "inputLayout": "PosNormTanUV",
        "material": "auto",
        "model": model,
        "name": f"Palm {index + 1:03d} ({variant})",
        "position": [round(component, 6) for component in position],
        "rotationDeg": [0.0, round(yaw_degrees, 4), 0.0],
        "scale": [round(scale, 4)] * 3,
        "shader": "shaders/gbuffer.hlsl",
        "type": "staticMesh",
    }


def populate(args: argparse.Namespace) -> None:
    with args.level.open("r", encoding="utf-8") as level_file:
        level = json.load(level_file)

    original_objects = level.get("objects")
    if not isinstance(original_objects, list):
        raise ValueError(f"Level has no object array: {args.level}")

    kept_objects = [obj for obj in original_objects if obj.get("model") not in PALM_MODELS]
    island_translation = _find_island_translation(kept_objects, args.mesh)
    props = _prop_positions(kept_objects)
    triangles = _read_surface_triangles(args.mesh, args.min_normal_y)
    cumulative_areas: list[float] = []
    total_area = 0.0
    for triangle in triangles:
        total_area += triangle.area
        cumulative_areas.append(total_area)

    rng = random.Random(args.seed)
    accepted: list[Vec3] = []
    minimum_distance_squared = args.min_separation * args.min_separation
    prop_clearance_squared = args.prop_clearance * args.prop_clearance

    for _ in range(args.max_attempts):
        triangle_index = bisect.bisect_left(cumulative_areas, rng.random() * total_area)
        local = _sample_triangle(triangles[min(triangle_index, len(triangles) - 1)], rng)
        world = (
            local[0] + island_translation[0],
            local[1] + island_translation[1],
            local[2] + island_translation[2],
        )
        if world[1] < args.min_height:
            continue

        angle = math.degrees(math.atan2(world[2], world[0])) % 360.0
        if _angle_delta_degrees(angle, args.inlet_angle) < args.inlet_half_angle:
            continue
        if any(_xz_distance_squared(world, placed) < minimum_distance_squared for placed in accepted):
            continue
        if any(_xz_distance_squared(world, prop) < prop_clearance_squared for prop in props):
            continue

        accepted.append(world)
        if len(accepted) == args.count:
            break

    if len(accepted) != args.count:
        raise RuntimeError(
            f"Placed only {len(accepted)} of {args.count} palms after "
            f"{args.max_attempts} attempts; reduce spacing or loosen filters"
        )

    models = [PALM_MODELS[index % len(PALM_MODELS)] for index in range(args.count)]
    rng.shuffle(models)
    next_id = max((int(obj.get("id", 0)) for obj in kept_objects), default=0) + 1
    palms = [
        _make_palm_object(
            next_id + index,
            index,
            models[index],
            position,
            rng.uniform(0.0, 360.0),
            rng.uniform(args.min_scale, args.max_scale),
        )
        for index, position in enumerate(accepted)
    ]

    level["objects"] = kept_objects + palms
    with args.level.open("w", encoding="utf-8", newline="\n") as level_file:
        json.dump(level, level_file, indent=2)
        level_file.write("\n")

    counts = {model: models.count(model) for model in PALM_MODELS}
    print(f"Wrote {args.count} palms to {args.level}")
    print(f"seed={args.seed} scale=[{args.min_scale}, {args.max_scale}]")
    print(f"minimum separation={args.min_separation} m, minimum height={args.min_height} m")
    for model, count in counts.items():
        print(f"  {model}: {count}")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--level", type=Path, default=DEFAULT_LEVEL)
    parser.add_argument("--mesh", type=Path, default=DEFAULT_MESH)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20260715)
    parser.add_argument("--min-scale", type=float, default=0.8)
    parser.add_argument("--max-scale", type=float, default=1.2)
    parser.add_argument("--min-height", type=float, default=0.45)
    parser.add_argument("--min-normal-y", type=float, default=0.82)
    parser.add_argument("--min-separation", type=float, default=6.0)
    parser.add_argument("--prop-clearance", type=float, default=7.0)
    parser.add_argument("--inlet-angle", type=float, default=90.0)
    parser.add_argument("--inlet-half-angle", type=float, default=50.0)
    parser.add_argument("--max-attempts", type=int, default=250000)
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count must not be negative")
    if not 0.0 < args.min_scale <= args.max_scale:
        parser.error("scale range must be positive and ordered")
    if not 0.0 <= args.min_normal_y <= 1.0:
        parser.error("--min-normal-y must be in [0, 1]")
    if args.min_separation < 0.0 or args.prop_clearance < 0.0:
        parser.error("clearance values must not be negative")
    return args


if __name__ == "__main__":
    populate(_parse_args())
