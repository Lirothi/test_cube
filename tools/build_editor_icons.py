"""Rasterize the viewport icons' SVGs and rebuild the atlas without resampling the icons it already has.

Layout (256 px cells): [dirlight | point], [spot | camera], [zone | player_start].
An SVG whose icon_<name>.png exists is not rasterized again (the browser's rendering may move); delete the PNG to redo it.
Run from any working directory: python tools/build_editor_icons.py
"""
from pathlib import Path
import argparse
import tempfile

from PIL import Image
import build_content_browser_icons as svg_rasterizer


ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = ROOT / "textures" / "editor"
SOURCES = ROOT / "art" / "editor" / "viewport_icons"
CELL = 256
LEGACY = ("dirlight", "point", "spot", "camera")
VECTOR = ("zone", "player_start")  # the cells after the legacy ones, in order


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", type=Path, help="Path to Edge or Chrome for SVG rasterization")
    args = parser.parse_args()
    # Reuse the project's SVG renderer; existing PNG cells remain untouched.
    svg_rasterizer.CELL_SIZE = CELL
    atlas = Image.new("RGBA", (CELL * 2, CELL * 3), (0, 0, 0, 0))
    for index, name in enumerate(LEGACY):
        with Image.open(OUTPUT_DIR / f"icon_{name}.png") as source:
            cell = source.convert("RGBA")
            if cell.size != (CELL, CELL):
                raise RuntimeError(f"Unexpected dimensions for icon_{name}.png: {cell.size}")
            atlas.paste(cell, ((index % 2) * CELL, (index // 2) * CELL))

    previous_path = OUTPUT_DIR / "editor_icons.png"
    with Image.open(previous_path) as previous:
        legacy_pixels = previous.convert("RGBA").crop((0, 0, 512, 512)).tobytes()
    if atlas.crop((0, 0, 512, 512)).tobytes() != legacy_pixels:
        raise RuntimeError("Legacy cells differ from the current atlas; refusing to replace it")

    with tempfile.TemporaryDirectory(prefix="viewport-icon-", ignore_cleanup_errors=True) as temp:
        folder = Path(temp).resolve()
        if not folder.is_relative_to(Path(tempfile.gettempdir()).resolve()):
            raise RuntimeError("Unexpected temporary directory")
        for index, name in enumerate(VECTOR, start=len(LEGACY)):
            target = OUTPUT_DIR / f"icon_{name}.png"
            if not target.exists():
                raster = folder / f"{name}.png"
                svg_rasterizer.rasterize(args.browser or svg_rasterizer.find_browser(), SOURCES / f"{name}.svg", raster, folder / "profile")
                with Image.open(raster) as image:
                    icon = image.convert("RGBA")
                    if icon.size != (CELL, CELL) or icon.getchannel("A").getbbox() is None:
                        raise RuntimeError(f"{name}.svg rasterization failed")
                    icon.save(target)
                print(f"Rasterized {name}.svg")
            with Image.open(target) as source:
                cell = source.convert("RGBA")
                if cell.size != (CELL, CELL):
                    raise RuntimeError(f"Unexpected dimensions for icon_{name}.png: {cell.size}")
                atlas.paste(cell, ((index % 2) * CELL, (index // 2) * CELL))

    atlas.save(previous_path, compress_level=9)
    with Image.open(previous_path) as saved:
        if saved.convert("RGBA").crop((0, 0, 512, 512)).tobytes() != legacy_pixels:
            raise RuntimeError("Saved atlas changed a legacy cell")
    print("Wrote editor_icons.png (512x768); all four legacy cells are pixel-identical")


if __name__ == "__main__":
    main()
