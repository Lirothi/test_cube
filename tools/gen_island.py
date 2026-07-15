#!/usr/bin/env python3
"""Generate the tropical-atoll island ring and lagoon-floor OBJ meshes.

The generator is deterministic and uses only the Python standard library.  Its
default island is an annular heightfield with roughly 83k triangles: the shores
cross y=0 gradually except for one submerged inlet, and the radial edges remain
underwater for ocean blending.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
from typing import Iterable, Sequence


Vec2 = tuple[float, float]
Vec3 = tuple[float, float, float]
Triangle = tuple[int, int, int]

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ISLAND = REPO_ROOT / "models" / "atoll_island.obj"
DEFAULT_LAGOON = REPO_ROOT / "models" / "atoll_lagoon_floor.obj"


def _seed_phase(seed: int, channel: int) -> float:
    """Return a reproducible phase in [0, 2*pi) without global RNG state."""
    value = (seed ^ (channel * 0x9E3779B9)) & 0xFFFFFFFF
    value ^= value >> 16
    value = (value * 0x7FEB352D) & 0xFFFFFFFF
    value ^= value >> 15
    value = (value * 0x846CA68B) & 0xFFFFFFFF
    value ^= value >> 16
    return (value / 0xFFFFFFFF) * math.tau


def _angular_noise(angle: float, seed: int, channel: int) -> float:
    """Low-frequency periodic noise suitable for a closed radial boundary."""
    return (
        0.52 * math.sin(2.0 * angle + _seed_phase(seed, channel * 3 + 0))
        + 0.31 * math.sin(5.0 * angle + _seed_phase(seed, channel * 3 + 1))
        + 0.17 * math.sin(8.0 * angle + _seed_phase(seed, channel * 3 + 2))
    )


def _surface_noise(x: float, z: float, seed: int) -> float:
    """Smooth broad undulation; no per-vertex white noise or faceted chatter."""
    return (
        0.50 * math.sin(0.105 * x + 0.071 * z + _seed_phase(seed, 20))
        + 0.31 * math.sin(-0.061 * x + 0.137 * z + _seed_phase(seed, 21))
        + 0.19 * math.sin(0.173 * x - 0.119 * z + _seed_phase(seed, 22))
    )


def _smoothstep(edge0: float, edge1: float, value: float) -> float:
    if edge0 == edge1:
        return 1.0 if value >= edge1 else 0.0
    t = max(0.0, min(1.0, (value - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def _cross(a: Vec3, b: Vec3) -> Vec3:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _sub(a: Vec3, b: Vec3) -> Vec3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _vertex_normals(vertices: Sequence[Vec3], triangles: Sequence[Triangle]) -> list[Vec3]:
    sums = [[0.0, 0.0, 0.0] for _ in vertices]
    for a, b, c in triangles:
        normal = _cross(_sub(vertices[b], vertices[a]), _sub(vertices[c], vertices[a]))
        for index in (a, b, c):
            sums[index][0] += normal[0]
            sums[index][1] += normal[1]
            sums[index][2] += normal[2]

    result: list[Vec3] = []
    for x, y, z in sums:
        length = math.sqrt(x * x + y * y + z * z)
        if length <= 1.0e-12:
            result.append((0.0, 1.0, 0.0))
        else:
            inv = 1.0 / length
            result.append((x * inv, y * inv, z * inv))
    return result


def _ring_radii(angle: float, seed: int) -> tuple[float, float, float]:
    # The dry atoll is exactly twice the first G0 draft in X/Z.  The final radius
    # is a separate, much longer underwater skirt rather than stretched dry land.
    inner = 36.0 + 3.2 * _angular_noise(angle, seed, 0)
    land_outer = 110.0 + 7.6 * _angular_noise(angle, seed, 1)
    skirt_outer = 185.0 + 14.0 * _angular_noise(angle, seed, 5)
    return inner, land_outer, skirt_outer


def _camp_center(seed: int, camp_angle_deg: float) -> tuple[float, float, float]:
    angle = math.radians(camp_angle_deg)
    inner, land_outer, _ = _ring_radii(angle, seed)
    radius = inner + (land_outer - inner) * 0.55
    return radius * math.cos(angle), 1.25, radius * math.sin(angle)


def generate_island(
    seed: int,
    angular_segments: int,
    radial_segments: int,
    camp_angle_deg: float,
    channel_angle_deg: float,
) -> tuple[list[Vec3], list[Vec2], list[Vec3], list[Triangle], Vec3]:
    vertices: list[Vec3] = []
    uvs: list[Vec2] = []
    triangles: list[Triangle] = []
    camp_x, camp_y, camp_z = _camp_center(seed, camp_angle_deg)
    camp_angle = math.radians(camp_angle_deg)
    camp_radial = (math.cos(camp_angle), math.sin(camp_angle))
    camp_tangent = (-camp_radial[1], camp_radial[0])
    channel_angle = math.radians(channel_angle_deg)
    uv_extent = 60.0
    land_row_fraction = 0.75

    for radial_index in range(radial_segments + 1):
        band = radial_index / radial_segments
        for angular_index in range(angular_segments):
            angle = math.tau * angular_index / angular_segments
            inner, land_outer, skirt_outer = _ring_radii(angle, seed)
            if band <= land_row_fraction:
                land_band = band / land_row_fraction
                skirt_band = 0.0
                radius = inner + (land_outer - inner) * land_band
            else:
                land_band = 1.0
                skirt_band = (band - land_row_fraction) / (1.0 - land_row_fraction)
                radius = land_outer + (skirt_outer - land_outer) * skirt_band
            x = radius * math.cos(angle)
            z = radius * math.sin(angle)

            crest = 0.55 + 0.025 * _angular_noise(angle, seed, 2)
            width = 0.245 + 0.012 * _angular_noise(angle, seed, 3)
            ring = math.exp(-((land_band - crest) / width) ** 2)
            y = -2.70 + 5.35 * ring
            y += 0.48 * _surface_noise(x, z, seed) * ring

            # Raise the lagoon-side underwater shelf by one metre, tapering the
            # correction away before the crest so the dry relief is unchanged.
            y += 1.0 * (1.0 - _smoothstep(0.0, 0.52, land_band))

            if skirt_band > 0.0:
                skirt_blend = _smoothstep(0.0, 1.0, skirt_band)
                skirt_depth = -12.0 + 0.8 * _angular_noise(angle, seed, 6)
                y = y * (1.0 - skirt_blend) + skirt_depth * skirt_blend

            # On the inlet side, replace the generic outer falloff with a broad
            # beach shelf.  It stays shallow for most of its reach, but still
            # reaches 10.5 m depth at the far skirt edge.  The sector has a wide
            # core and blends into the steeper coast on either side.
            beach_delta = abs(
                math.atan2(
                    math.sin(angle - channel_angle), math.cos(angle - channel_angle)
                )
            )
            beach_weight = 1.0 - _smoothstep(
                math.radians(40.0), math.radians(70.0), beach_delta
            )
            outer_shore_band = min(
                0.95,
                crest + width * math.sqrt(-math.log(2.70 / 5.35)),
            )
            outer_shore_radius = inner + (land_outer - inner) * outer_shore_band
            if beach_weight > 0.0 and radius >= outer_shore_radius:
                beach_progress = max(
                    0.0,
                    min(
                        1.0,
                        (radius - outer_shore_radius)
                        / (skirt_outer - outer_shore_radius),
                    ),
                )
                beach_y = -10.5 * beach_progress**1.8
                y = y * (1.0 - beach_weight) + beach_y * beach_weight

            # A broad pad on one arc gives the later rock cave and fire a stable floor.
            dx = x - camp_x
            dz = z - camp_z
            radial_distance = dx * camp_radial[0] + dz * camp_radial[1]
            tangent_distance = dx * camp_tangent[0] + dz * camp_tangent[1]
            elliptical_distance = math.sqrt(
                (radial_distance / 11.0) ** 2 + (tangent_distance / 16.0) ** 2
            )
            flatten = 1.0 - _smoothstep(0.52, 1.0, elliptical_distance)
            y = y * (1.0 - flatten) + camp_y * flatten

            # Open the ring to the ocean.  The indicated channel widens toward
            # the outside, stays at least 0.8 m below sea level throughout its
            # core, and feathers into the banks outside the authored width.
            channel_delta = math.atan2(
                math.sin(angle - channel_angle), math.cos(angle - channel_angle)
            )
            if abs(channel_delta) < math.radians(30.0):
                channel_progress = max(
                    0.0, min(1.0, (radius - inner) / (land_outer - inner))
                )
                channel_half_width = 8.0 + 10.0 * _smoothstep(
                    0.0, 1.0, channel_progress
                )
                channel_cross_distance = abs(radius * math.sin(channel_delta))
                channel_weight = 1.0 - _smoothstep(
                    channel_half_width,
                    channel_half_width + 2.0,
                    channel_cross_distance,
                )
                channel_floor = min(y, -0.80)
                y = y * (1.0 - channel_weight) + channel_floor * channel_weight

            vertices.append((x, y, z))
            uvs.append((x / (2.0 * uv_extent) + 0.5, z / (2.0 * uv_extent) + 0.5))

    for radial_index in range(radial_segments):
        row = radial_index * angular_segments
        next_row = (radial_index + 1) * angular_segments
        for angular_index in range(angular_segments):
            next_angle = (angular_index + 1) % angular_segments
            inner_now = row + angular_index
            inner_next = row + next_angle
            outer_now = next_row + angular_index
            outer_next = next_row + next_angle

            # Standard OBJ counter-clockwise winding viewed from above. MeshManager
            # converts it to the engine's clockwise D3D convention on load.
            triangles.append((inner_now, inner_next, outer_next))
            triangles.append((inner_now, outer_next, outer_now))

    normals = _vertex_normals(vertices, triangles)
    return vertices, uvs, normals, triangles, (camp_x, camp_y, camp_z)


def generate_lagoon_floor(
    seed: int,
    segments: int,
) -> tuple[list[Vec3], list[Vec2], list[Vec3], list[Triangle]]:
    # The disc overlaps the atoll's inner underwater edge. Its edge is slightly
    # lower, so the rising island slope hides the join without z-fighting.
    radius = 56.0
    height = -1.55
    uv_extent = 60.0
    vertices: list[Vec3] = [(0.0, height, 0.0)]
    uvs: list[Vec2] = [(0.5, 0.5)]

    for index in range(segments):
        angle = math.tau * index / segments
        # Very small low-frequency variation avoids a visibly perfect shoreline disc.
        edge = radius + 0.9 * _angular_noise(angle, seed, 4)
        x = edge * math.cos(angle)
        z = edge * math.sin(angle)
        vertices.append((x, height, z))
        uvs.append((x / (2.0 * uv_extent) + 0.5, z / (2.0 * uv_extent) + 0.5))

    triangles: list[Triangle] = []
    for index in range(segments):
        current = index + 1
        following = ((index + 1) % segments) + 1
        triangles.append((0, following, current))

    normals = [(0.0, 1.0, 0.0)] * len(vertices)
    return vertices, uvs, normals, triangles


def _write_obj(
    path: Path,
    object_name: str,
    vertices: Sequence[Vec3],
    uvs: Sequence[Vec2],
    normals: Sequence[Vec3],
    triangles: Iterable[Triangle],
    comments: Sequence[str],
) -> None:
    if not (len(vertices) == len(uvs) == len(normals)):
        raise ValueError("OBJ vertex, UV, and normal arrays must have identical lengths")

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as obj:
        obj.write("# Procedurally generated by tools/gen_island.py\n")
        for comment in comments:
            obj.write(f"# {comment}\n")
        obj.write(f"o {object_name}\n")
        for x, y, z in vertices:
            obj.write(f"v {x:.6f} {y:.6f} {z:.6f}\n")
        for u, v in uvs:
            obj.write(f"vt {u:.7f} {v:.7f}\n")
        for x, y, z in normals:
            obj.write(f"vn {x:.7f} {y:.7f} {z:.7f}\n")
        obj.write("s 1\n")
        for a, b, c in triangles:
            # All three streams share an index, making the engine parser keep one vertex.
            a += 1
            b += 1
            c += 1
            obj.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")
    temporary.replace(path)


def _validate_mesh(
    name: str,
    vertices: Sequence[Vec3],
    uvs: Sequence[Vec2],
    normals: Sequence[Vec3],
    triangles: Sequence[Triangle],
) -> None:
    if not vertices or not triangles:
        raise ValueError(f"{name} mesh is empty")
    if not (len(vertices) == len(uvs) == len(normals)):
        raise ValueError(f"{name} vertex streams have different lengths")

    for vertex in vertices:
        if not all(math.isfinite(component) for component in vertex):
            raise ValueError(f"{name} contains a non-finite vertex")
    for normal in normals:
        if not all(math.isfinite(component) for component in normal):
            raise ValueError(f"{name} contains a non-finite normal")
        if normal[1] <= 0.0:
            raise ValueError(f"{name} contains a downward-facing vertex normal")

    vertex_count = len(vertices)
    for triangle in triangles:
        if any(index < 0 or index >= vertex_count for index in triangle):
            raise ValueError(f"{name} contains an out-of-range triangle index")
        a, b, c = triangle
        area_vector = _cross(_sub(vertices[b], vertices[a]), _sub(vertices[c], vertices[a]))
        area_squared = sum(component * component for component in area_vector)
        if area_squared <= 1.0e-18:
            raise ValueError(f"{name} contains a degenerate triangle")


def _validate_water_access(
    vertices: Sequence[Vec3],
    angular_segments: int,
    radial_segments: int,
    channel_angle_deg: float,
) -> None:
    channel_angle = math.radians(channel_angle_deg)
    for angular_index in range(angular_segments):
        heights = [
            vertices[radial_index * angular_segments + angular_index][1]
            for radial_index in range(radial_segments + 1)
        ]
        if heights[0] >= 0.0 or heights[-1] >= 0.0:
            raise ValueError(
                "island must begin and end underwater "
                f"at angular column {angular_index}"
            )
        angle = math.tau * angular_index / angular_segments
        channel_delta = abs(
            math.atan2(
                math.sin(angle - channel_angle), math.cos(angle - channel_angle)
            )
        )
        if channel_delta <= math.radians(1.0) and max(heights) >= 0.0:
            raise ValueError("water channel centerline must remain below sea level")
        if channel_delta >= math.radians(22.0) and max(heights) <= 0.0:
            raise ValueError(
                "island must rise above sea level outside the water channel "
                f"at angular column {angular_index}"
            )


def _validate_beach_profile(
    vertices: Sequence[Vec3],
    angular_segments: int,
    radial_segments: int,
    channel_angle_deg: float,
) -> None:
    # Sample inside the beach sector but outside the channel itself.
    sample_angle = math.radians(channel_angle_deg + 25.0)
    angular_index = round(sample_angle / math.tau * angular_segments) % angular_segments
    column = [
        vertices[radial_index * angular_segments + angular_index]
        for radial_index in range(radial_segments + 1)
    ]
    dry_indices = [index for index, vertex in enumerate(column) if vertex[1] >= 0.0]
    if not dry_indices:
        raise ValueError("beach validation column never rises above sea level")

    shore_index = max(dry_indices)
    shore_radius = math.hypot(column[shore_index][0], column[shore_index][2])
    edge_radius = math.hypot(column[-1][0], column[-1][2])
    middle_radius = (shore_radius + edge_radius) * 0.5
    middle_vertex = min(
        column[shore_index:],
        key=lambda vertex: abs(math.hypot(vertex[0], vertex[2]) - middle_radius),
    )
    if middle_vertex[1] < -4.5:
        raise ValueError("beach shelf gains depth too quickly")
    if column[-1][1] > -10.0:
        raise ValueError("beach shelf must reach at least 10 m depth at its outer edge")


def _mesh_summary(name: str, vertices: Sequence[Vec3], triangles: Sequence[Triangle]) -> str:
    xs = [vertex[0] for vertex in vertices]
    ys = [vertex[1] for vertex in vertices]
    zs = [vertex[2] for vertex in vertices]
    return (
        f"{name}: {len(vertices):,} vertices, {len(triangles):,} triangles, "
        f"bounds x=[{min(xs):.2f}, {max(xs):.2f}] "
        f"y=[{min(ys):.2f}, {max(ys):.2f}] "
        f"z=[{min(zs):.2f}, {max(zs):.2f}]"
    )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1337, help="deterministic shape seed")
    parser.add_argument("--angular-segments", type=int, default=288)
    parser.add_argument("--radial-segments", type=int, default=144)
    parser.add_argument("--lagoon-segments", type=int, default=256)
    parser.add_argument(
        "--camp-angle-deg",
        type=float,
        default=30.0,
        help="angle of the flattened cave/camp pad around the ring",
    )
    parser.add_argument(
        "--channel-angle-deg",
        type=float,
        default=90.0,
        help="direction of the below-sea-level ocean inlet (+Z by default)",
    )
    parser.add_argument("--island-output", type=Path, default=DEFAULT_ISLAND)
    parser.add_argument("--lagoon-output", type=Path, default=DEFAULT_LAGOON)
    args = parser.parse_args()
    if args.angular_segments < 32:
        parser.error("--angular-segments must be at least 32")
    if args.radial_segments < 16:
        parser.error("--radial-segments must be at least 16")
    if args.lagoon_segments < 16:
        parser.error("--lagoon-segments must be at least 16")
    return args


def main() -> int:
    args = _parse_args()
    island = generate_island(
        args.seed,
        args.angular_segments,
        args.radial_segments,
        args.camp_angle_deg,
        args.channel_angle_deg,
    )
    island_vertices, island_uvs, island_normals, island_triangles, camp_center = island
    lagoon_vertices, lagoon_uvs, lagoon_normals, lagoon_triangles = generate_lagoon_floor(
        args.seed,
        args.lagoon_segments,
    )

    _validate_mesh(
        "island", island_vertices, island_uvs, island_normals, island_triangles
    )
    _validate_mesh(
        "lagoon", lagoon_vertices, lagoon_uvs, lagoon_normals, lagoon_triangles
    )
    _validate_water_access(
        island_vertices,
        args.angular_segments,
        args.radial_segments,
        args.channel_angle_deg,
    )
    _validate_beach_profile(
        island_vertices,
        args.angular_segments,
        args.radial_segments,
        args.channel_angle_deg,
    )

    _write_obj(
        args.island_output,
        "atoll_island",
        island_vertices,
        island_uvs,
        island_normals,
        island_triangles,
        (
            f"seed={args.seed}",
            "sea level is y=0; channel centerline remains below y=0",
            f"water channel angle={args.channel_angle_deg:.3f} degrees",
            "outer beach sector stays shallow and reaches y=-10.5 at the skirt edge",
            "top-down UVs span 120 m; use level texOffsScale to set sand tiling",
            f"flattened camp pad center=({camp_center[0]:.3f}, {camp_center[1]:.3f}, {camp_center[2]:.3f})",
        ),
    )
    _write_obj(
        args.lagoon_output,
        "atoll_lagoon_floor",
        lagoon_vertices,
        lagoon_uvs,
        lagoon_normals,
        lagoon_triangles,
        (
            f"seed={args.seed}",
            "lagoon floor sits below sea level at y=-1.55",
            "top-down UVs share the island's 120 m projection",
        ),
    )

    print(_mesh_summary("island", island_vertices, island_triangles))
    print(_mesh_summary("lagoon", lagoon_vertices, lagoon_triangles))
    print(
        "camp pad center: "
        f"({camp_center[0]:.3f}, {camp_center[1]:.3f}, {camp_center[2]:.3f})"
    )
    print(f"wrote {args.island_output}")
    print(f"wrote {args.lagoon_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
