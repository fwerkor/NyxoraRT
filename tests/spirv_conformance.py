#!/usr/bin/env python3
"""Deterministic scalar CPU reference + SPIR-V validation smoke test."""
import json
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

MASK = (1 << 32) - 1


def run(*args, ok=True):
    result = subprocess.run(args, capture_output=True, text=True, check=False)
    if ok and result.returncode:
        raise RuntimeError(f"{args}: {result.stderr}")
    return result


def source(rng, regs):
    raw = rng.choice([*range(12), *range(128, 145), *range(193, 209), 255])
    return raw


def value(raw, literal, regs):
    if raw <= 101:
        return regs[raw]
    if raw <= 192:
        return raw - 128
    if raw <= 208:
        return (192 - raw) & MASK
    return literal


def make_program(rng, length):
    regs = [0] * 102
    scc = False
    code = []
    for _ in range(length):
        op = rng.choice(("movk", "mov", "not", "add", "sub", "and", "or", "xor", "cmp", "nop"))
        dst = rng.randrange(12)
        if op == "nop":
            code.append(0xBF800000)
            continue
        if op == "movk":
            signed = rng.randrange(-32768, 32768)
            regs[dst] = signed & MASK
            code.append(0xB0000000 | (dst << 16) | (signed & 0xFFFF))
            continue
        src0 = source(rng, regs)
        src1 = source(rng, regs) if op in ("add", "sub", "and", "or", "xor", "cmp") else None
        literal = rng.getrandbits(32) if (src0 == 255 or src1 == 255) else 0
        lhs = value(src0, literal, regs)
        rhs = value(src1, literal, regs) if src1 is not None else None
        if op == "mov":
            code.append(0xBE800000 | (dst << 16) | (3 << 8) | src0)
            regs[dst] = lhs
        elif op == "not":
            code.append(0xBE800000 | (dst << 16) | (7 << 8) | src0)
            regs[dst] = (~lhs) & MASK
            scc = regs[dst] != 0
        elif op == "cmp":
            code.append(0xBF000000 | (6 << 16) | (src1 << 8) | src0)
            scc = lhs == rhs
        else:
            selector = {"add": 0, "sub": 1, "and": 0x0E,
                        "or": 0x10, "xor": 0x12}[op]
            code.append(0x80000000 | (selector << 23) |
                        (dst << 16) | (src1 << 8) | src0)
            if op == "add":
                result = (lhs + rhs) & MASK
                scc = result < lhs
            elif op == "sub":
                result = (lhs - rhs) & MASK
                scc = lhs >= rhs
            elif op == "and":
                result = lhs & rhs
                scc = result != 0
            elif op == "or":
                result = lhs | rhs
                scc = result != 0
            else:
                result = lhs ^ rhs
                scc = result != 0
            regs[dst] = result
        if src0 == 255 or src1 == 255:
            code.append(literal)
    code.append(0xBF810000)
    return code, regs, int(scc)


def main(executable, validator, vulkan_runner=None):
    rng = random.Random(20261119)
    with tempfile.TemporaryDirectory(prefix="nyxora-spirv-") as temp:
        root = Path(temp)
        source_path = root / "shader.bin"
        output_path = root / "shader.spv"
        for count in range(32):
            words, expected_registers, expected_scc = make_program(rng, count + 1)
            source_path.write_bytes(struct.pack(f"<{len(words)}I", *words))
            execution = json.loads(run(executable, "--shader-exec", str(source_path)).stdout)
            assert execution["sgpr"] == expected_registers, f"case {count}: SGPR mismatch"
            assert execution["scc"] == expected_scc, f"case {count}: SCC mismatch"
            decoded = json.loads(run(executable, "--shader-scalar", str(source_path)).stdout)
            assert decoded["dwords"] == len(words)
            graph = json.loads(run(executable, "--shader-cfg", str(source_path)).stdout)
            assert len(graph["blocks"]) == 1 and graph["blocks"][0]["reachable"]
            run(executable, "--shader-spv", str(source_path), str(output_path))
            assert output_path.read_bytes()[:4] == b"\x03\x02\x23\x07"
            run(validator, "--target-env", "vulkan1.1", str(output_path))
            if vulkan_runner:
                result = json.loads(run(vulkan_runner, str(output_path)).stdout)
                assert result["sgpr"] == expected_registers, f"Vulkan case {count}: SGPR mismatch"
                assert result["scc"] == expected_scc, f"Vulkan case {count}: SCC mismatch"

        # Branches are valid CFGs but cannot be translated as straight-line ALU.
        source_path.write_bytes(struct.pack("<2I", 0xBF820000, 0xBF810000))
        graph = json.loads(run(executable, "--shader-cfg", str(source_path)).stdout)
        assert len(graph["blocks"]) == 2
        output_path.unlink(missing_ok=True)
        rejected = run(executable, "--shader-spv", str(source_path), str(output_path), ok=False)
        assert rejected.returncode != 0 and not output_path.exists()
    print("32 deterministic scalar programs: CPU, SPIR-V" +
          (", Vulkan execution" if vulkan_runner else "") + " conformance passed")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        raise SystemExit("usage: spirv_conformance.py NYXORA_BINARY SPIRV_VAL_BINARY [VULKAN_RUNNER]")
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) == 4 else None)
