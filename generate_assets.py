#!/usr/bin/env python3
"""Generate deterministic, responsive SVG documentation assets.

No plotting package or network access is required. The fallback rate-distortion
chart is explicitly labelled illustrative; measured benchmark JSON is never
silently replaced with invented measurements.
"""

from __future__ import annotations

import json
import math
from collections.abc import Iterable
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "assets"
RESULTS = ROOT / "benchmarks" / "results" / "benchmark_results.json"
BACKGROUND = "#0b1020"
PANEL = "#151d33"
TEXT = "#e6edf7"
MUTED = "#93a4bd"
CYAN = "#37d5ff"
VIOLET = "#a78bfa"
GREEN = "#55e6a5"
ORANGE = "#ffb454"


def frame(width: int, height: int, body: str, title: str) -> str:
    return f"""<svg xmlns="http://www.w3.org/2000/svg" role="img" aria-labelledby="title desc"
 viewBox="0 0 {width} {height}" width="100%" height="auto">
<title id="title">{title}</title><desc id="desc">AetherStream documentation graphic</desc>
<defs><filter id="shadow"><feDropShadow dx="0" dy="5" stdDeviation="8" flood-opacity=".35"/></filter>
<marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z" fill="{CYAN}"/></marker></defs>
<rect width="100%" height="100%" rx="18" fill="{BACKGROUND}"/>
{body}</svg>\n"""


def architecture() -> str:
    labels = [
        ("float32", "telemetry stream"),
        ("ADSP", "mode selector"),
        ("closed loop", "predictor"),
        ("online GED", "estimator"),
        ("SIMD cubic", "ECLM quantizer"),
        ("16-way", "interleaved rANS"),
        ("indexed bytes", "CRC32-C + AIDX"),
    ]
    pieces = [
        f'<text x="700" y="48" text-anchor="middle" fill="{TEXT}" font-family="system-ui" font-size="25" font-weight="700">AetherStream compression pipeline</text>'
    ]
    box_width, gap, start, y = 160, 34, 40, 92
    for index, (top, bottom) in enumerate(labels):
        x = start + index * (box_width + gap)
        color = CYAN if index in (0, 6) else (VIOLET if index in (1, 2) else GREEN)
        pieces.append(
            f'<rect x="{x}" y="{y}" width="{box_width}" height="104" rx="14" fill="{PANEL}" stroke="{color}" stroke-width="2" filter="url(#shadow)"/>'
        )
        pieces.append(
            f'<text x="{x + box_width / 2}" y="{y + 43}" text-anchor="middle" fill="{TEXT}" font-family="system-ui" font-size="17" font-weight="700">{top}</text>'
        )
        pieces.append(
            f'<text x="{x + box_width / 2}" y="{y + 70}" text-anchor="middle" fill="{MUTED}" font-family="system-ui" font-size="13">{bottom}</text>'
        )
        if index + 1 < len(labels):
            pieces.append(
                f'<path d="M{x + box_width + 5} {y + 52}H{x + box_width + gap - 7}" stroke="{CYAN}" stroke-width="3" marker-end="url(#arrow)"/>'
            )
    pieces.append(
        f'<path d="M520 222 C520 274 365 274 365 205" fill="none" stroke="{ORANGE}" stroke-width="2" stroke-dasharray="7 5" marker-end="url(#arrow)"/>'
    )
    pieces.append(
        f'<text x="442" y="264" text-anchor="middle" fill="{ORANGE}" font-family="system-ui" font-size="13">reconstructed-state feedback</text>'
    )
    return frame(1400, 300, "".join(pieces), "AetherStream architecture pipeline")


def polyline(points: Iterable[tuple[float, float]], color: str, width: int = 3) -> str:
    encoded = " ".join(f"{x:.1f},{y:.1f}" for x, y in points)
    return f'<polyline points="{encoded}" fill="none" stroke="{color}" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round"/>'


def rate_distortion() -> str:
    # These reference points communicate chart semantics, not measured claims.
    series: dict[str, tuple[list[float], list[float], str]] = {
        "AetherStream v2.2": ([2.0, 3.0, 4.0], [38, 48, 57], CYAN),
        "SZ3": ([2.4, 3.5, 4.7], [36, 45, 52], GREEN),
        "Zstandard shuffled": ([17.0, 20.0, 23.0], [60, 60, 60], ORANGE),
        "Uniform quantizer": ([2.0, 3.0, 4.0], [31, 39, 45], VIOLET),
    }
    measured = False
    if RESULTS.exists():
        try:
            rows = json.loads(RESULTS.read_text(encoding="utf-8"))
            grouped: dict[str, list[dict[str, Any]]] = {}
            for row in rows:
                codec = str(row.get("codec", ""))
                if codec.startswith("aether-rate") and row.get("psnr_db") is not None:
                    grouped.setdefault(codec, []).append(row)
            if grouped:
                points = []
                for codec in sorted(grouped, key=lambda name: float(name.rsplit("-", 1)[1])):
                    codec_rows = grouped[codec]
                    points.append(
                        (
                            sum(float(row["bits_per_sample"]) for row in codec_rows)
                            / len(codec_rows),
                            sum(float(row["psnr_db"]) for row in codec_rows) / len(codec_rows),
                        )
                    )
                series["AetherStream v2.2"] = (
                    [point[0] for point in points],
                    [point[1] for point in points],
                    CYAN,
                )
                measured = True
        except (ValueError, KeyError, TypeError):
            measured = False
    left, top, plot_w, plot_h = 90, 72, 760, 360
    max_x, min_y, max_y = 24.0, 25.0, 65.0
    body = [
        f'<text x="470" y="38" text-anchor="middle" fill="{TEXT}" font-family="system-ui" font-size="23" font-weight="700">Rate–distortion comparison</text>'
    ]
    for tick in range(0, 25, 4):
        x = left + tick / max_x * plot_w
        body.append(
            f'<path d="M{x} {top}V{top + plot_h}" stroke="#26324e"/><text x="{x}" y="{top + plot_h + 25}" text-anchor="middle" fill="{MUTED}" font-family="system-ui" font-size="12">{tick}</text>'
        )
    for tick in range(30, 66, 10):
        y = top + (max_y - tick) / (max_y - min_y) * plot_h
        body.append(
            f'<path d="M{left} {y}H{left + plot_w}" stroke="#26324e"/><text x="{left - 14}" y="{y + 4}" text-anchor="end" fill="{MUTED}" font-family="system-ui" font-size="12">{tick}</text>'
        )
    for index, (name, (xs, ys, color)) in enumerate(series.items()):
        points = [
            (left + x / max_x * plot_w, top + (max_y - y) / (max_y - min_y) * plot_h)
            for x, y in zip(xs, ys)
        ]
        body.append(polyline(points, color))
        for x, y in points:
            body.append(f'<circle cx="{x}" cy="{y}" r="5" fill="{color}"/>')
        legend_y = 102 + index * 31
        body.append(
            f'<path d="M875 {legend_y}h28" stroke="{color}" stroke-width="4"/><text x="914" y="{legend_y + 5}" fill="{TEXT}" font-family="system-ui" font-size="13">{name}</text>'
        )
    label = (
        "measured AetherStream rows; comparison references illustrative"
        if measured
        else "illustrative reference curves — run benchmarks/bench_rigorous.py for measurements"
    )
    body.append(
        f'<text x="470" y="480" text-anchor="middle" fill="{MUTED}" font-family="system-ui" font-size="12">{label}</text>'
    )
    body.append(
        f'<text x="470" y="458" text-anchor="middle" fill="{TEXT}" font-family="system-ui" font-size="14">wire bits / sample</text><text transform="translate(25 250) rotate(-90)" text-anchor="middle" fill="{TEXT}" font-family="system-ui" font-size="14">PSNR (dB)</text>'
    )
    return frame(1100, 505, "".join(body), "Rate distortion chart")


def spectral() -> str:
    width, height = 1100, 420
    samples = 180
    original, closed, open_loop, error = [], [], [], []
    drift = 0.0
    for i in range(samples):
        t = i / 18.0
        signal = math.exp(-t / 16.0) * math.sin(2.4 * t) + 0.22 * math.sin(0.45 * t)
        quantum = 0.045 * math.sin(5.7 * t)
        drift += quantum * 0.06
        restored = signal - quantum
        x = 55 + i * 5.45
        original.append((x, 125 - signal * 67))
        closed.append((x, 125 - restored * 67))
        open_loop.append((x, 300 - (signal - drift) * 48))
        error.append((x, 300 - quantum * 250))
    body = [
        f'<text x="550" y="36" text-anchor="middle" fill="{TEXT}" font-family="system-ui" font-size="23" font-weight="700">Closed-loop prediction prevents cumulative drift</text>',
        polyline(original, MUTED, 5),
        polyline(closed, CYAN, 2),
        polyline(open_loop, ORANGE, 3),
        polyline(error, GREEN, 2),
        f'<text x="65" y="63" fill="{MUTED}" font-family="system-ui" font-size="13">oscillatory transient: original (gray), reconstructed (cyan)</text>',
        f'<text x="65" y="232" fill="{ORANGE}" font-family="system-ui" font-size="13">open-loop accumulated drift (orange) versus instantaneous error −qₜ (green)</text>',
        f'<text x="550" y="390" text-anchor="middle" fill="{TEXT}" font-family="serif" font-size="22">εₜ = xₜ − x̃ₜ = −qₜ</text>',
    ]
    return frame(width, height, "".join(body), "Closed-loop spectral predictor")


def main() -> None:
    ASSETS.mkdir(exist_ok=True)
    outputs = {
        "architecture_pipeline.svg": architecture(),
        "rate_distortion_curve.svg": rate_distortion(),
        "spectral_predictor.svg": spectral(),
    }
    for name, content in outputs.items():
        path = ASSETS / name
        path.write_text(content, encoding="utf-8")
        print(f"wrote {path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
