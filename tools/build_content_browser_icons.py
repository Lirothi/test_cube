from pathlib import Path
import subprocess
import tempfile

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "art" / "editor" / "content_browser_icons"
OUTPUT_PATH = ROOT / "textures" / "editor" / "content_browser_icons.png"
CELL_SIZE = 64
ICONS = ("folder", "level", "shader", "unknown", "preview_failed")


def find_browser() -> Path:
    candidates = (
        Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
        Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"),
        Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError("Edge or Chrome is required to rasterize the SVG icons")


def rasterize(browser: Path, source: Path, output: Path, profile: Path) -> None:
    subprocess.run(
        (
            str(browser),
            "--headless=new",
            "--disable-gpu",
            "--hide-scrollbars",
            "--force-device-scale-factor=1",
            "--default-background-color=00000000",
            f"--window-size={CELL_SIZE},{CELL_SIZE}",
            f"--user-data-dir={profile}",
            f"--screenshot={output}",
            source.resolve().as_uri(),
        ),
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def main() -> None:
    browser = find_browser()
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="content-browser-icons-") as temp:
        temp_dir = Path(temp)
        profile = temp_dir / "browser-profile"
        atlas = Image.new("RGBA", (CELL_SIZE * len(ICONS), CELL_SIZE), (0, 0, 0, 0))

        for index, name in enumerate(ICONS):
            raster_path = temp_dir / f"{name}.png"
            rasterize(browser, SOURCE_DIR / f"{name}.svg", raster_path, profile)
            with Image.open(raster_path) as image:
                cell = image.convert("RGBA")
                if cell.size != (CELL_SIZE, CELL_SIZE):
                    raise RuntimeError(f"{name}.svg rasterized to unexpected size {cell.size}")
                atlas.alpha_composite(cell, (index * CELL_SIZE, 0))

        atlas.save(OUTPUT_PATH, format="PNG", optimize=False, compress_level=9)

    print(f"Wrote {OUTPUT_PATH.relative_to(ROOT)} ({len(ICONS)} cells)")


if __name__ == "__main__":
    main()
