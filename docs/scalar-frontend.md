# Typed AMD scalar frontend and diagnostic SPIR-V pipeline

This milestone extends the **synthetic/homebrew diagnostic path**. It is not a
PS5 commercial-title renderer. All supported encodings and execution semantics
are explicitly whitelisted; unknown guest instructions are rejected.

## Scalar bytecode and control flow

The mixed-width decoder recognizes selected GCN scalar instructions:

| Family | Supported operations |
| --- | --- |
| SOPP | NOP, ENDPGM, BRANCH, SCC/VCC/EXEC conditional branches, WAITCNT, BARRIER |
| SOP1 | MOV_B32, NOT_B32 |
| SOP2 | ADD_U32, SUB_U32, AND_B32, OR_B32, XOR_B32 |
| SOPC | CMP_EQ_U32 |
| SOPK | MOVK_I32 |

Sources support SGPR 0–101, 0–64 inline integer constants, −1 through −16
inline integers, and an additional 32-bit little-endian literal dword.
Unrecognized scalar source encodings, unsupported destination registers,
malformed or truncated literal operands, and branches into literal dwords
are rejected. The bytecode must end with S_ENDPGM. This strict scope is
important: disassembly support alone is not evidence of successful translation
for SIMD/vector, LDS, VMEM, image, interpolation or export instructions.

`build_scalar_cfg` computes instruction-boundary basic blocks, backedges,
forward edges, predecessor lists, and reachability. The graph can represent
branching and loops even though the first SPIR-V lowerer does not yet
translate them.

## CPU reference executor

`execute_scalar_subset` interprets a restricted subset of the scalar
operations: SGPR writes, SCC from comparison, arithmetic carry/no-borrow
and boolean results, and SCC0/SCC1/always branches. It uses a bounded
instruction-step budget to stop malformed infinite loops. VCC/EXEC branches,
WAITCNT, BARRIER and other wave or GPU synchronisation instructions are
rejected. This is a verification reference, not a CPU fallback for guest
graphics.

## Initial Vulkan 1.1 SPIR-V

`compile_scalar_diagnostic_spirv` accepts only **straight-line** programs
of NOP, scalar ALU, CMP_EQ_U32 and ENDPGM instructions (at most 4,096
instructions). All branches, barriers, waits, and other guest GPU effects
are rejected *before* code generation.

The generated SPIR-V 1.3 compute entry point uses a single invocation per
workgroup and one `StorageBuffer` descriptor:

- Descriptor set 0, binding 0: host-provided **103 uint32 values**, arranged
  as SGPR[0..101] followed by SCC at index 102, with 4-byte array stride.
- The kernel loads current SGPR inputs and writes back the affected registers
  and SCC. Buffer contents must be initialized by the caller. The diagnostic
  dispatch must contain **exactly one** workgroup to avoid racing on the
  shared register buffer.
- This buffer ABI is for testing only; it is not the console's real
  wavefront/register allocation model and is not a full compute backend.

The little-endian shader/CFG and SPIR-V CLI commands are:

```bash
./build/nyxora --shader-scalar ./program.bin
./build/nyxora --shader-cfg ./program.bin
./build/nyxora --shader-exec ./program.bin
./build/nyxora --shader-spv ./program.bin ./program.spv
spirv-val --target-env vulkan1.1 ./program.spv
```

`--shader-scalar`, `--shader-cfg`, and `--shader-exec` output JSON;
`--shader-spv` writes a raw SPIR-V module. The legacy `--shader-sopp`
command remains intact.

## Verification

The `nyxora_tests` CTest suite includes scalar encoding/literal tests,
CFG reachability/backedge tests, an instruction-bounded reference execution
loop, arithmetic/SCC cases, and SPIR-V structure/rejection checks. When
`spirv-val` and Python are available, CTest registers an additional
`nyxora_spirv_conformance` test that checks **32 seeded synthetic programs**:
their CPU SGPR/SCC outcomes are compared to a separate Python semantic
reference, the CLI disassembly/CFG are checked, and every emitted module
is validated for Vulkan 1.1 by Khronos SPIR-V Tools.

The GPU module was also validated manually against Vulkan 1.1, 1.2 and
1.3 with Khronos `spirv-val`. Binary validation is not evidence of
successful Vulkan execution or hardware rendering.

## Remaining integration

Actual work toward rendering games requires a much broader scalar/vector
ISA decoder, typed SSA IR, structured control-flow lowering, descriptor and
shader resource tracking, wave semantics, texture/image/geometry support,
real queue synchronisation, shader/pipeline caches, and Vulkan resource,
presentation and memory coherency. Avoid silently replacing unsupported
instructions with NOPs.

Research sources and design comparisons:

- [AMD RDNA Shader ISA](https://developer.amd.com/wp-content/resources/RDNA_Shader_ISA.pdf)
- [Khronos SPIR-V specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html)
- KytyPS5 and shadPS4 public source layouts for stage separation; no
  third-party GPL source code was copied into NyxoraRT.
