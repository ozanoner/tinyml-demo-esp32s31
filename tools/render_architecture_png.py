#!/usr/bin/env python3
"""Render an archify HTML artifact to a static PNG.

GitHub does not render an interactive HTML blob, so the README needs a raster
render of the same drawing. The artifact embeds its own <svg> plus a
theme-variable stylesheet in the document head; this script rebuilds a minimal
page that keeps exactly those two pieces and the diagram's own viewBox, then
screenshots it with headless Chrome at 2x so the PNG stays legible at
GitHub's content width.

Usage:
    render_png.py <artifact.html> <out.png> [scale]
"""

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CHROME = (Path.home() / ".cache/ms-playwright/chromium-1228/chrome-linux64/chrome")
CHROME_FLAGS = ["--no-sandbox", "--disable-dev-shm-usage", "--headless=new",
                "--hide-scrollbars", "--default-background-color=ffffffff"]


def main() -> int:
    artifact = Path(sys.argv[1]).resolve()
    out_png = Path(sys.argv[2]).resolve()
    scale = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0

    html = artifact.read_text(encoding="utf-8")

    svg_start = html.index("<svg")
    svg_end = html.index("</svg>") + len("</svg>")
    svg = html[svg_start:svg_end]

    view_box = re.search(r'viewBox="0 0 ([0-9.]+) ([0-9.]+)"', svg)
    if not view_box:
        raise SystemExit("no viewBox in the artifact's <svg>")
    width, height = float(view_box.group(1)), float(view_box.group(2))

    # The artifact's head carries the theme variables the svg's own <style>
    # refers to (color-mix(...) over --mask, --*-stroke, ...). Keep the first two
    # style blocks - fonts and theme - and drop the viewer/runtime CSS, which is
    # what draws the toolbar the PNG must not contain.
    styles = re.findall(r"<style[^>]*>.*?</style>", html[:svg_start], re.S)
    keep = [s for s in styles if "archify-fonts" in s[:120]] or []
    theme = [s for s in styles if s not in keep][:1]
    head_styles = "\n".join(keep + theme)

    html_attrs = 'lang="en" data-theme="dark" data-preset="classic"'
    page = (
        f"<!doctype html><html {html_attrs}><head><meta charset=\"utf-8\">"
        f"{head_styles}"
        "<style>html,body{margin:0;padding:0;background:#0b1020;}"
        f"svg{{display:block;width:{width}px;height:{height}px;}}</style>"
        f"</head><body>{svg}</body></html>"
    )

    with tempfile.TemporaryDirectory() as tmp:
        page_path = Path(tmp) / "page.html"
        page_path.write_text(page, encoding="utf-8")
        cmd = [
            str(CHROME), *CHROME_FLAGS,
            f"--force-device-scale-factor={scale}",
            f"--window-size={int(width)},{int(height)}",
            f"--screenshot={out_png}",
            page_path.as_uri(),
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if not out_png.exists():
            sys.stderr.write(proc.stderr[-2000:])
            return 1

    print(json.dumps({"png": str(out_png), "viewBox": [width, height],
                      "scale": scale, "bytes": out_png.stat().st_size}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
