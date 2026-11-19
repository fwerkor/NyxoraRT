#!/usr/bin/env python3
"""Seeded independent VOP1/VOP2 32-lane reference and Vulkan differential test."""
import json
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

MASK = 0xffffffff
LANES = 32
COUNT = 256


def checked(*args):
    result = subprocess.run(args, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed: {args}: {result.stderr}")
    return result.stdout


def source(rng):
    return rng.choice([*(256 + i for i in range(24)),
                       *range(128, 145), *range(193, 209), 255])


def source_value(encoded, literal, state, lane):
    if encoded >= 256:
        return state[(encoded - 256) * LANES + lane]
    if encoded <= 192:
        return encoded - 128
    if encoded <= 208:
        return (192 - encoded) & MASK
    return literal


def vop1(op, dst, src):
    return 0x7e000000 | (dst << 17) | (op << 9) | src


def vop2(op, dst, src, rhs_vgpr):
    return (op << 25) | (dst << 17) | (rhs_vgpr << 9) | src


def generate(rng, count):
    state = [(((i * 1664525 + 1013904223) & MASK) ^ 0x9e3779b9)
             for i in range(COUNT * LANES)]
    dwords = []
    modes = ("mov", "not", "add", "sub", "and", "or", "xor",
             "min", "max", "lshl", "lshr", "nop")
    opcodes = {"add": 0x25, "sub": 0x26, "and": 0x1b, "or": 0x1c,
               "xor": 0x1d, "min": 0x13, "max": 0x14,
               "lshl": 0x1a, "lshr": 0x16}

    for _ in range(count):
        op = rng.choice(modes)
        if op == "nop":
            dwords.append(vop1(0, 0, 0))
            continue
        dst = rng.randrange(24)
        first = source(rng)
        second = rng.randrange(24) if op not in ("mov", "not") else None
        literal = rng.getrandbits(32) if first == 255 else 0
        if op in ("mov", "not"):
            dwords.append(vop1(1 if op == "mov" else 0x37, dst, first))
        else:
            dwords.append(vop2(opcodes[op], dst, first, second))
        if first == 255:
            dwords.append(literal)
        for lane in range(LANES):
            lhs = source_value(first, literal, state, lane)
            rhs = state[second * LANES + lane] if second is not None else 0
            if op == "mov":
                result = lhs
            elif op == "not":
                result = ~lhs
            elif op == "add":
                result = lhs + rhs
            elif op == "sub":
                result = lhs - rhs
            elif op == "and":
                result = lhs & rhs
            elif op == "or":
                result = lhs | rhs
            elif op == "xor":
                result = lhs ^ rhs
            elif op == "min":
                result = min(lhs, rhs)
            elif op == "max":
                result = max(lhs, rhs)
            elif op == "lshl":
                result = rhs << (lhs & 31)
            else:
                result = rhs >> (lhs & 31)
            state[dst * LANES + lane] = result & MASK

    dwords.append(0xbf810000)
    return dwords, state


def main(nyxora, validator, vulkan=None):
    rng = random.Random(20261120)
    with tempfile.TemporaryDirectory(prefix="nyxora-valu-") as directory:
        directory = Path(directory)
        code_file = directory / "vector.bin"
        spirv_file = directory / "vector.spv"
        for case in range(32):
            code, expected = generate(rng, case + 1)
            code_file.write_bytes(struct.pack(f"<{len(code)}I", *code))
            decoded = json.loads(checked(nyxora, "--shader-valu", str(code_file)))
            assert decoded["dwords"] == len(code), (case, "length mismatch")
            assert decoded["lanes"] == LANES

            scalar_reference = json.loads(
                checked(nyxora, "--shader-valu-exec-seeded", str(code_file))
            )
            assert scalar_reference["vgpr"] == expected, (case, "C++ CPU reference mismatch")
            assert scalar_reference["lanes"] == LANES

            checked(nyxora, "--shader-valu-spv", str(code_file), str(spirv_file))
            checked(validator, "--target-env", "vulkan1.1", str(spirv_file))
            if vulkan:
                result = json.loads(checked(vulkan, "--vector-seeded", str(spirv_file)))
                assert result["vgpr"] == expected, (case, "Vulkan per-lane VGPR mismatch")
                assert result["lanes"] == LANES

        code_file.write_bytes(struct.pack("<2I", vop1(1, 0, 0), 0xbf810000))
        spirv_file.unlink(missing_ok=True)
        rejected = subprocess.run([nyxora, "--shader-valu-spv", str(code_file),
                                   str(spirv_file)], capture_output=True, text=True)
        assert rejected.returncode != 0 and not spirv_file.exists()

    print("32 seeded VOP1/VOP2 shaders: CPU and SPIR-V" +
          (", Vulkan 32-lane execution" if vulkan else "") + " passed")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        raise SystemExit("usage: vector_conformance.py NYXORA SPIRV_VAL [VULKAN_RUNNER]")
    main(*sys.argv[1:])
