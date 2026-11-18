# PM4 trace replay

NyxoraRT can validate and normalize **one** raw PM4 submission at a time, with no GPU or
Vulkan dependency:

```bash
./build/nyxora --pm4-replay graphics graphics.pm4
./build/nyxora --pm4-replay compute compute.pm4
```

The input is a *headerless* sequence of 32-bit **little-endian** PM4 command dwords.
It is not an ELF, shader dump, firmware image, proprietary capture container, or
entire GPU queue recording. Use only traces obtained from inputs you may lawfully
inspect. A capture tool must remove its own metadata and delimit individual
submissions before replaying them here.

The output is one JSON object per invocation. It contains the queue, the number of
input dwords and decoded packets, plus an ordered array of normalized commands.
Guest addresses are hexadecimal strings, retaining the full 64-bit value even in
JavaScript JSON tooling. Shader bindings are snapshots at draw or dispatch time,
so later register writes cannot retroactively alter an earlier command.

For example, an indexed draw emits a `draw_index_2` command carrying
`index_address`, `index_count`, `max_size`, `initiator`, raw
`index_type` (or `null` if unset), and optional vertex/pixel shader bindings.

The parser rejects truncated packets, unsupported opcodes/semantics, and input
files larger than 1,048,576 dwords (4 MiB). Failed submissions do not commit
partial PM4 register state. The process exits nonzero on errors.

## Important limitations

- A trace is processed as a single independent submission with initially empty
  register state. The CLI does not yet accept ordered multi-submission captures.
- The frontend does **not** fetch shader bytecode, read index-buffer memory,
  perform host rendering, or emulate GPU scheduling, memory barriers, and
  synchronization. GPU register snapshots alone cannot execute a game.
- Graphics and compute use separate PM4 state models; CLI compute traces use
  logical queue ID 0 in the output.
- Only known register windows and PM4 packet semantics are supported. An
  unsupported packet is reported, not silently skipped.

Regression coverage includes little-endian decoding, exact size limits,
precision-safe JSON addresses, compute dispatch, and rejected packets.
