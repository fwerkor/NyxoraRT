# GPU guest resource snapshots and SOPP decoding

This milestone bridges the PM4 shader/index bindings to the runtime's
`GuestAddressSpace` in a deterministic, host-only diagnostic path. It does
not yet submit GPU work to Vulkan.

## Guest resource snapshot

`GuestMemoryReader` copies at most **4 MiB** per request and checks the
canonical guest region table before dereferencing memory. Adjacent mappings
can be read in one request; unmapped gaps, uncommitted regions, absent
read permission, addresses that overflow 64-bit space, and excessive
requests fail without returning a partial snapshot. This protects native
backings that would fault if `GuestAddressSpace::view` were used on a
`PROT_NONE` region.

`resolve_index_buffer` takes the typed `DrawIndex2` command produced by
the PM4 frontend, and resolves the raw `VGT_INDEX_TYPE` encoding:

| Index format | Raw value | Bytes per element |
| --- | --- | --- |
| 16-bit unsigned | 0 | 2 |
| 32-bit unsigned | 1 | 4 |
| 8-bit unsigned | 2 | 1 |

The decoder reads indices in **little-endian** order and returns 32-bit
host values. Unknown formats, unaligned guest addresses, missing format
state, out-of-range index counts and inaccessible memory fail. No implicit
buffer uploads, draw calls or GPU synchronization are performed.

`snapshot_shader_words` reads an explicit requested number of 32-bit
little-endian words from a PM4 shader binding; it does not infer program
length from PM4 resource metadata.

## First scalar ISA decoder

`shader::decode_sopp_program` is deliberately strict and only parses
32-bit scalar SOPP opcodes: `S_NOP`, `S_ENDPGM`, `S_BRANCH`,
`S_CBRANCH_{SCC0,SCC1,VCCZ,VCCNZ,EXECZ,EXECNZ}`, `S_BARRIER`, and
`S_WAITCNT`. Decoded records preserve signed immediate and verified
word-relative branch target. Input is bounded to 65,536 dwords and must
terminate in `S_ENDPGM`. Any unknown instruction or out-of-bounds branch
causes rejection instead of pretending the shader has been translated.

`shader::decode_guest_sopp_program` combines a memory snapshot with
this decoder. The integration test exercises a synthetic PM4
`SET_SH_REG` + `DISPATCH_DIRECT` submission, resolves the resulting
CS address against read/execute guest memory and decodes its test program.
A separate graphics submission resolves a 16-bit index buffer.

**Limitations:** Real RDNA shaders contain multiple instruction families,
mix 32- and 64-bit encodings, and need a CFG/IR/resource model and full
SPIR-V legalization. These remain unsupported. Guest VM state is read
synchronously; concurrent guest writes, host/GPU coherency, direct-memory
aliasing, and real GPU-address translation are not implemented. This is a
test/debugging primitive, not a working PS5 graphics renderer.

## References

Opcode and hardware-index-format numbers were checked against public
AMD/Mesa documentation and independently implemented in NyxoraRT:

- [AMD RDNA 1.0 ISA documentation](https://developer.amd.com/wp-content/resources/RDNA_Shader_ISA.pdf)
- [Mesa gfx103 GPU register definitions](https://chromium.googlesource.com/chromiumos/third_party/mesa/+/a11d1bd247df18057ecb9945e01bf39ff85d8d9d/src/amd/registers/gfx103.json)

KytyPS5's public source layout was reviewed for architectural module
boundaries. No code from GPL projects was copied.
