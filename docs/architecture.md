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

PT_TLS templates are registered per loaded module. Each guest thread receives its own aligned copy of initialized TLS bytes plus zeroed TLS BSS and a 0x40-byte TCB whose DTV uses the guest module identifiers assigned by the linker. Moving a thread context explicitly rebinds the TCB self/DTV pointers so the ABI structure cannot retain stale host addresses.

Executable file-backed segments are decoded with pinned Zydis 4.1.1 before final RX protection. NyxoraRT only recognizes the narrow TCB forms currently needed by the ABI: 64-bit `MOV`, `CMP`, or `XOR` from `FS:[disp]` with no base/index and a displacement inside the TCB. This avoids blind byte replacement: a literal `0x64` inside an immediate or displacement is never mistaken for an FS prefix. Decode failures advance conservatively by one byte so mixed code/data executable segments can still be inspected. A recognized form that cannot be represented safely on the current host is a load-time error rather than an implicit compatibility guess.

On Linux x86-64, supported guest FS accesses are rewritten to GS and a scoped segment binding places the guest TCB at the GS base while leaving host FS untouched. On Windows x64, the runtime reserves one of the first 64 Win32 TLS slots. Guest `FS:[0]` is still rewritten in place to the corresponding `GS:[TEB.TlsSlots+n]` address. Supported nonzero 64-bit `MOV`, `CMP`, and `XOR` reads use a nearby RX side thunk: the original instruction becomes a five-byte near jump, the thunk reloads the real TCB pointer from the TEB slot, dereferences the decoded TCB offset, performs the original operation, and jumps back. `CMP`/`XOR` scratch storage lives below the SysV red zone and cleanup uses flag-preserving instructions, so the original condition codes reach following guest instructions. The current patch arena is one host page per module; exhaustion and operand forms that cannot be preserved safely are load-time errors rather than silent approximations.

POSIX x86-64 native execution installs process-level SIGSEGV/SIGBUS/SIGILL handlers once, while Windows x64 installs a vectored exception handler. Capture state is thread-local and only active around native guest invocation. Both paths record the fault address and instruction pointer, rewrite the native exception context to the entry trampoline's saved host stack/recovery epilogue, and return through normal register restoration. POSIX faults outside an active guest invocation are chained to the previous handler; Windows returns `EXCEPTION_CONTINUE_SEARCH`.

`GuestThread` composes an independent guest stack, TLS/TCB context, segment scope, entry trampoline, and captured result on a real host thread. `GuestThreadManager` owns opaque guest thread handles per runtime and propagates both the manager and current handle through a thread-local scope, so `pthread_self` is stable from the first guest instruction and a child can create another thread without a process-global runtime singleton. `pthread_create`, `pthread_join`, `pthread_self`, and `pthread_detach` are registered for both `libkernel` and `libScePosix`; attributes remain limited to the null/default case. Guest detach marks the handle non-joinable but intentionally keeps the host `std::thread` runtime-owned. Finished detached records are reaped lazily, and manager shutdown prevents new creates before joining any remaining workers outside the manager lock. Windows HLE calls use one generated SysV-to-MS-x64 bridge that remaps the first four integer/pointer arguments rather than per-function bridge implementations.

The next native-execution work is:

- multi-page or demand-grown Windows CPU-patch arenas plus rarer TCB operand forms that cannot use the current scratch template;
- pthread attributes, cancellation basics, and stronger thread-state diagnostics; add `pthread_exit` only after an explicit guest-termination recovery path can return through the native trampoline without C++ unwinding across raw guest/generated frames;
- Windows host-call stack switching for HLE functions that may probe or consume substantial stack;
- a narrow fallback mechanism for instructions or behaviors that cannot execute directly.

The CPU layer must not become a general DBT unless evidence shows it is necessary.

### Initial libkernel HLE

The HLE registry shares the same `SymbolKey` space as guest exports. The first registered `libkernel` functions cover process time, process-time counter/frequency, current-CPU queries, and the initial pthread lifecycle. On SysV x86-64 hosts these bindings can point directly at host implementations. Windows uses a generated guest-SysV-to-host-MS-x64 bridge with shadow-space handling and remaps the first four integer/pointer argument registers; signatures wider than that still need an explicit bridge extension rather than unsafe function-pointer casts.

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
