# VOP1/VOP2 32-lane Vulkan vector diagnostics

NyxoraRT now implements a strict AMD GCN/RDNA integer vector ALU subset
independent of its SGPR/SCC scalar path. Real VOP1/VOP2 instruction fields
and little-endian 32-bit literals are decoded, while unsupported opcodes
and unmapped wave state are rejected.

## Supported instruction subset

| Family | Operations |
| --- | --- |
| VOP1 | V_NOP, V_MOV_B32, V_NOT_B32 |
| VOP2 | V_ADD_NC_U32, V_SUB_NC_U32, V_AND_B32, V_OR_B32, V_XOR_B32, V_MIN_U32, V_MAX_U32, V_LSHLREV_B32, V_LSHRREV_B32 |
| SOPP | S_ENDPGM (only at program end) |

The 9-bit source `src0` accepts VGPR0..255 (codes 256..511),
unsigned inline constants 0..64, inline negative constants -1..-16,
or one extra literal dword (code 255). VOP2 `vsrc1` selects another
VGPR; SGPR/EXEC/VCC/SDWA/DPP are explicitly not supported.

This initial diagnostic model has exactly **32 lanes**, storing 256 VGPRs
for each lane in one 8192-element uint32 buffer indexed as
`vgpr[register * 32 + lane]`.

## CLI commands

```bash
./build/nyxora --shader-valu ./vector.bin
./build/nyxora --shader-valu-exec ./vector.bin
./build/nyxora --shader-valu-exec-seeded ./vector.bin
./build/nyxora --shader-valu-spv ./vector.bin ./vector.spv
spirv-val --target-env vulkan1.1 ./vector.spv
```

The seeded CPU mode uses distinct deterministic initial values for
every register and lane, enabling tests to catch accidental broadcast
or incorrect register indexing.

To run the diagnostic Vulkan computation:

```bash
cmake -S . -B build-vulkan -DNYXORA_ENABLE_VULKAN_DIAGNOSTICS=ON
cmake --build build-vulkan --parallel
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build-vulkan/nyxora_vulkan_scalar --vector-seeded ./vector.spv
```

The Vulkan module uses **one workgroup with 32 invocations**, using
`LocalInvocationIndex` to select a lane. Descriptor set 0, binding 0
holds the host-initialized 8192 uint32 VGPR words. Dispatch exactly
one workgroup: no inter-workgroup or inter-lane synchronization is
provided.

## Tests

`tests/vector_conformance.py` generates 32 deterministic programs
with increasing instruction lengths and simulates all **8192 per-lane
register values** in an independent Python implementation. Each program
is checked by the C++ reference interpreter and Khronos spirv-val; the
optional Vulkan build additionally executes every kernel on Mesa Lavapipe
and reads all 8192 values back for equality checks.

The optional CI job runs both scalar and vector Vulkan tests. Default
Linux/Windows/macOS builds do not require the Vulkan SDK.

## Limitations

This milestone has no proper EXEC/VCC masks, scalar-to-vector sources,
floating-point vector ALU, memory/texture/LDS/DS instructions,
interpolation/export, wavefront scheduling, pixel/vertex programs,
graphics rasterization or PS5 game compatibility. Unrecognized
instructions are rejected rather than silently treated as NOPs.

The implementation was independently written after consulting public
AMD RDNA ISA and KytyPS5 architectural references; no GPL code copied.
