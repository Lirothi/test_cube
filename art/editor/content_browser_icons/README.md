# Content Browser Icons

Editable SVG sources for resources that do not have real thumbnails.

Atlas cell order:

1. `folder`
2. `level`
3. `shader`
4. `unknown`
5. `preview_failed`

Each source is rasterized to a transparent 64x64 cell. Rebuild the committed
runtime atlas from the repository root:

```powershell
python tools/build_content_browser_icons.py
```

The script uses an installed Edge or Chrome in headless mode to rasterize the
SVGs and Pillow to assemble the atlas.

Runtime code loads only `textures/editor/content_browser_icons.png`.
