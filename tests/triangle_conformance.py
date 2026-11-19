#!/usr/bin/env python3
"""Real Vulkan offscreen 64x64 rasterization from a normalized PM4 draw."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def pm4_packet(opcode, payload_count):
    return (3 << 30) | ((payload_count - 1) << 16) | (opcode << 8)


def launch(renderer, trace, vertex, fragment, ppm):
    return subprocess.run([renderer, str(trace), vertex, fragment, str(ppm)],
                          capture_output=True, text=True, timeout=35)


def main(renderer, vertex, fragment):
    with tempfile.TemporaryDirectory(prefix="nyxora-pm4-triangle-") as directory:
        root = Path(directory)
        trace = root / "draw.pm4"
        output = root / "offscreen.ppm"

        # Typed PM4 frontend has exactly one valid DRAW_INDEX_AUTO of three vertices.
        trace.write_bytes(struct.pack("<3I", pm4_packet(0x2D, 2), 3, 0))
        result = launch(renderer, trace, vertex, fragment, output)
        assert result.returncode == 0, result.stderr
        data = output.read_bytes()
        header = b"P6\n64 64\n255\n"
        assert data.startswith(header), "unexpected PPM header"
        rgb = data[len(header):]
        assert len(rgb) == 64 * 64 * 3

        def pixel(x, y):
            offset = (y * 64 + x) * 3
            return tuple(rgb[offset:offset + 3])

        # Avoid depending on GPU vendor-specific rasterized edge samples.
        assert pixel(32, 32) == (255, 0, 0), "triangle did not cover center"
        for corner in ((0, 0), (63, 0), (0, 63), (63, 63)):
            assert pixel(*corner) == (0, 0, 0), ("bad clear/pixel at", corner)
        red_pixels = sum(rgb[i:i + 3] == b"\xff\x00\x00"
                         for i in range(0, len(rgb), 3))
        assert 800 <= red_pixels <= 1400, red_pixels

        bad_traces = [
            ("missing payload", struct.pack("<2I", pm4_packet(0x2D, 2), 3)),
            ("bad vertex count", struct.pack("<3I", pm4_packet(0x2D, 2), 4, 0)),
            ("unsupported compute packet", struct.pack("<3I",
                                                        pm4_packet(0x15, 2), 1, 1)),
            ("unknown packet", struct.pack("<2I", pm4_packet(0x7B, 1), 0)),
            ("predicated", struct.pack("<3I", pm4_packet(0x2D, 2) | 1, 3, 0)),
        ]
        for label, raw in bad_traces:
            trace.write_bytes(raw)
            output.unlink(missing_ok=True)
            rejected = launch(renderer, trace, vertex, fragment, output)
            assert rejected.returncode != 0, (label, rejected.stdout)
            assert not output.exists(), (label, "output incorrectly created")

    print(f"PM4 -> Vulkan graphics -> PPM: passed; red pixels={red_pixels}; "
          f"{len(bad_traces)} invalid traces rejected")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: triangle_conformance.py RENDERER VERTEX_SPV FRAGMENT_SPV")
    main(*sys.argv[1:])
