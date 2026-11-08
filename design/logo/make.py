"""Axiom logo masters ("Axiom A"): an A whose crossbar is one period of a soft-clipped sine,
in ember on a deep warm tile. The plugin draws the same geometry from Source/DesignTokens.h
(logoLegs / logoWave); keep the two in step.

python3 design/logo/make.py   ->  docs/logo.svg, assets/axiom-mark.svg, assets/axiom-icon-{512,1024}.png
(PNG icons need ImageMagick: `magick`.)
"""
import math
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
TILE, LEGS, WAVE = "#1F1A16", "#F2ECE3", "#F08A3C"


def wave_path():
    pts = []
    for i in range(49):
        t = i / 48
        x = 22.9 + (41.1 - 22.9) * t
        y = 38 - 5.4 * math.tanh(2.8 * math.sin(2 * math.pi * t)) / math.tanh(2.8)
        pts.append(f"{x:.2f} {y:.2f}")
    return "M" + " L".join(pts)


def svg(size=None):
    wh = f' width="{size}" height="{size}"' if size else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"{wh} role="img" aria-label="Axiom">'
            f'<rect width="64" height="64" rx="15" fill="{TILE}"/>'
            f'<path d="M17 48 32 15.5 47 48" fill="none" stroke="{LEGS}" stroke-width="4.5" stroke-linecap="round" stroke-linejoin="round"/>'
            f'<path d="{wave_path()}" fill="none" stroke="{WAVE}" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round"/></svg>\n')


(ROOT / "docs/logo.svg").write_text(svg())
(ROOT / "assets/axiom-mark.svg").write_text(svg(256))
for px, inner in ((1024, 832), (512, 416)):
    # macOS icons keep a transparent margin around the rounded tile.
    subprocess.run(["magick", "-background", "none", "-density", "900", str(ROOT / "assets/axiom-mark.svg"),
                    "-resize", f"{inner}x{inner}", "-gravity", "center", "-extent", f"{px}x{px}",
                    str(ROOT / f"assets/axiom-icon-{px}.png")], check=True)
print("done")
