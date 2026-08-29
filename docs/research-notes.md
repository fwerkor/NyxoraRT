# Reference-code study

These notes capture architectural observations from public emulator source trees inspected before the first NyxoraRT commit. They are design references, not copied code.

## KytyPS5

Inspected revision: `6980e08`.

Useful observations:

- The codebase cleanly separates `loader`, `kernel`, `libs`, `guest_gpu`, `host_gpu`, and shader-recompiler responsibilities.
- `runtimeLinker.cpp` demonstrates why relocation, TLS, guest-stack entry, and unresolved-import handling belong to one runtime-linking subsystem rather than the raw ELF parser.
- Its recent unresolved-import thunk path is especially useful conceptually: late resolution is a better compatibility-development primitive than eagerly turning every missing import into a fatal error.
- GPU processing has a visible guest command-processor boundary before Vulkan scheduling. That separation is essential for deterministic command tests and future trace replay.
- Shader compilation uses a multi-stage frontend/IR/backend structure rather than emitting host shaders directly from packet state.

## shadPS4

Inspected revision: `388cff5`.

Useful observations:

- `Linker::Relocate` keeps ELF relocation semantics explicit and records which relocations are already resolved.
- Symbol resolution includes library/module metadata rather than treating a symbol string as globally unique.
- The memory manager distinguishes physical/direct/flexible concepts from VMAs and connects renderer invalidation through a deliberate interface.
- The Vulkan rasterizer, caches, scheduler, and shader recompiler demonstrate the scale required for a mature AAA-oriented graphics path.
- Its renderer makes timeline/resource synchronization a first-class concern rather than a later optimization.

## RPCSX

Inspected revision: `e8ae148`.

Useful observations:

- RPCSX contains substantially more kernel/system modeling than the other references and explicitly distinguishes PS4 and PS5 process/syscall vectors.
- The VM layer integrates guest faults and GPU page state, showing that eventual GPU coherency work cannot be solved solely in the renderer.
- For NyxoraRT, this is primarily a behavioral/reference source. Adopting its full-system architecture would work against the narrower user-mode/HLE goal.

## Resulting NyxoraRT decisions

1. Keep parsing, runtime linking, memory policy, and GPU translation as separate libraries/subsystems.
2. Make direct x86-64 execution the architectural center; do not start from a CPU interpreter.
3. Make symbol keys version-aware from day one.
4. Build a deterministic GPU submission interface before Vulkan implementation.
5. Keep title quirks out of core primitives.
6. Implement new code independently. No source from the GPL reference projects is incorporated into NyxoraRT.

## Implemented runtime-linker milestone

The first implementation derived only format and architectural facts from the references, then reimplemented the behavior independently. NyxoraRT now parses SCE dynamic-table offsets from `PT_SCE_DYNLIBDATA`, decodes packed module/library identifiers, models PT_TLS metadata, applies the core x86-64 RELA forms used by the references, and keeps unresolved imports retryable.

The memory layer now has an identity-mapped native arena in addition to deterministic buffered storage. The same runtime-linker tests run against the shared memory policy, and an end-to-end synthetic SCE fixture verifies load -> relocate -> W^X protection -> native entry execution without introducing a CPU interpreter.
