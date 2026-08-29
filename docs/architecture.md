# Architecture

## Core principle

The target console family and desktop PCs share x86-64 CPU semantics. NyxoraRT therefore treats CPU emulation as an exception, not the default. The desired steady-state path is:

```text
Guest executable
  -> ELF/module loader
  -> relocations + symbol resolution
  -> identity-mapped guest virtual memory
  -> native x86-64 execution
       | system/library call
       v
     HLE thunk -> host implementation
       | GPU submit
       v
     PM4 frontend -> GPU state/IR -> Vulkan
```

This is closer to a compatibility layer than to a cycle-accurate machine emulator.

## Boundaries

### Loader

The loader owns binary parsing only. It produces validated structural information and never owns host OS mappings. This keeps SELF/container handling, ELF parsing, dynamic metadata, and relocation logic testable with byte buffers.

### Guest memory

`GuestAddressSpace` is the policy boundary for guest mappings. The initial implementation is a deterministic storage-backed model. The next backend will support identity mappings so guest pointers can remain valid native pointers during direct execution.

Memory metadata must remain independent of the Vulkan caches. GPU dirty tracking and page-fault/watch mechanisms should subscribe to mapping changes rather than become the canonical memory model.

### Symbols and HLE

A symbol is not identified by a NID alone. Resolution includes library/module identity, versions, and symbol kind. Resolution order will eventually be:

1. local/guest definition when ELF binding requires it;
2. matching export in an already loaded guest module;
3. registered HLE implementation;
4. optional late-binding thunk for unresolved functions;
5. deterministic unresolved-import diagnostic.

Late thunks are important for rapid compatibility development: unsupported functions can be observed without silently corrupting relocation state.

### Native CPU execution

Direct execution requires more than jumping to the ELF entry point. The native executor will own:

- identity-mapped virtual memory;
- guest stack creation and stack switching;
- SysV calling-convention entry/exit trampolines;
- FS/TLS handling per guest thread;
- signal/exception routing for guest faults;
- a narrow fallback mechanism for instructions or behaviors that cannot execute directly.

The CPU layer must not become a general DBT unless evidence shows it is necessary.

### GPU

The GPU is intentionally separated into five stages:

```text
Guest submission
  -> command packet decoder
  -> architectural GPU state
  -> draw/dispatch operations + shader discovery
  -> host backend (Vulkan first)
```

Shader translation is a separate pipeline:

```text
Guest shader ISA
  -> decoded CFG
  -> typed SSA-like IR
  -> legalization/optimization
  -> SPIR-V
  -> persistent shader + pipeline cache
```

The command frontend must be deterministic and usable without Vulkan. That enables trace replay, fuzzing, unit tests, and remote differential testing later.

### Compatibility layer

Title-specific behavior belongs in a quirk/patch layer keyed by executable identity. Low-level memory, symbol, and command-decoding code must not accumulate title checks.

## Performance constraints

The architecture is designed around these non-negotiable constraints:

- no per-instruction CPU dispatch on the normal path;
- no compulsory guest-memory copies on CPU access;
- no synchronous shader compilation after a cache hit;
- timeline-based GPU synchronization instead of host-wide stalls;
- resource caches keyed by guest identity/state with explicit invalidation;
- tracing/profiling that can be compiled out or left off on the hot path.

## Testing strategy

The project will favor deterministic unit inputs before commercial software:

- synthetic SCE-style ELF images;
- homebrew executables;
- isolated relocation/TLS tests;
- PM4 packet fixtures and trace replay;
- shader corpus tests;
- differential behavior records produced by external contributors on hardware they own.
