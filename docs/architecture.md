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

`GuestAddressSpace` is the policy boundary for guest mappings. It can use deterministic storage-backed regions or a reserved `NativeArena` whose guest addresses are the actual host virtual addresses. The runtime and linker use the same interface in both modes. Native regions are loaded writable and then transitioned to their final permissions; relocation patching temporarily removes execute permission rather than creating RWX pages.

Native mappings currently require page-aligned region bases. Handling overlapping ELF protection ranges within one host page remains a later VM-policy concern rather than something the loader should special-case.

Memory metadata must remain independent of the Vulkan caches. GPU dirty tracking and page-fault/watch mechanisms should subscribe to mapping changes rather than become the canonical memory model.

### Symbols and HLE

A symbol is not identified by a NID alone. Resolution includes library/module identity, versions, and symbol kind. The current linker resolves local definitions, loaded guest exports, and registered HLE bindings using NID + library/module identity + versions + symbol kind. Guest exports may replace an HLE binding for the same exact key. Missing imports are preserved in a `RelocationReport`, and the module can be relinked after another module or HLE implementation becomes available.

Unresolved callable imports can now be patched to a stable late-binding thunk. Each thunk has immutable RX code and a separate RW slot containing its eventual target and an unresolved-call counter. Before binding it returns a deterministic zero value and records first use; after a symbol becomes available, relinking updates both the original relocation and the thunk target so cached thunk pointers remain valid.

### Native CPU execution

The native-memory path can load and relocate a synthetic SCE image, build a guarded guest stack, switch `RSP`/`RBP`, present the guest SysV argument convention, and return safely to the host. `Runtime::invoke_entry()` composes this with a per-thread runtime context. On Windows x64 the generated entry bridge preserves Microsoft-ABI nonvolatile GPR/XMM state while entering SysV guest code.

PT_TLS templates are registered per loaded module. Each guest thread receives its own aligned copy of initialized TLS bytes plus zeroed TLS BSS. The current thread context is explicit and nestable; it deliberately does not replace the host FS/GS base yet. Real binaries that directly issue segment-relative TLS accesses therefore still need the segment/TLS binding layer.

The next native-execution work is:

- safe guest segment/TLS binding without corrupting host runtime TLS;
- signal/exception routing for guest faults;
- persistent guest-thread ownership and creation/join;
- Windows host-call stack switching for HLE functions that may probe or consume substantial stack;
- a narrow fallback mechanism for instructions or behaviors that cannot execute directly.

The CPU layer must not become a general DBT unless evidence shows it is necessary.

### Initial libkernel HLE

The HLE registry shares the same `SymbolKey` space as guest exports. The first registered `libkernel` functions cover process time, process-time counter/frequency, and current-CPU queries. On SysV x86-64 hosts these bindings can point directly at host implementations. Windows uses a generated guest-SysV-to-host-MS-x64 no-argument bridge with shadow-space handling; wider signatures will use explicit ABI bridge families rather than unsafe function-pointer casts.

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
