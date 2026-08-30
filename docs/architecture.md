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

The native-memory path can load and relocate a synthetic SCE image, build a guarded guest stack, switch `RSP`/`RBP`, present the guest SysV argument convention, and return safely to the host. `Runtime::invoke_entry()` composes this with a per-thread runtime context. On Windows x64 the generated entry bridge preserves Microsoft-ABI nonvolatile GPR/XMM state while entering SysV guest code. It also publishes the suspended OS-stack pointer through a dedicated inline TEB TLS slot. Generated HLE bridges preserve the guest stack pointer/nonvolatile state, switch back to that OS stack for the C++ host call (including normal Windows shadow-space/alignment), then return to the guest stack. The entry trampoline restores the previous TLS slot on both normal return and fault recovery, so nested native invocations do not lose the outer host-stack context.

PT_TLS templates are registered per loaded module. Each guest thread receives its own aligned copy of initialized TLS bytes plus zeroed TLS BSS and a 0x40-byte TCB whose DTV uses the guest module identifiers assigned by the linker. Moving a thread context explicitly rebinds the TCB self/DTV pointers so the ABI structure cannot retain stale host addresses.

Executable file-backed segments are decoded with pinned Zydis 4.1.1 before final RX protection. NyxoraRT only recognizes the narrow TCB forms currently needed by the ABI: 64-bit `MOV`, `CMP`, or `XOR` from `FS:[disp]` with no base/index and a displacement inside the TCB. This avoids blind byte replacement: a literal `0x64` inside an immediate or displacement is never mistaken for an FS prefix. Decode failures advance conservatively by one byte so mixed code/data executable segments can still be inspected. A recognized form that cannot be represented safely on the current host is a load-time error rather than an implicit compatibility guess.

On Linux x86-64, supported guest FS accesses are rewritten to GS and a scoped segment binding places the guest TCB at the GS base while leaving host FS untouched. On Windows x64, the runtime reserves one of the first 64 Win32 TLS slots. Guest `FS:[0]` is still rewritten in place to the corresponding `GS:[TEB.TlsSlots+n]` address. Supported nonzero 64-bit `MOV`, `CMP`, and `XOR` reads use a nearby RX side thunk: the original instruction becomes a five-byte near jump, the thunk reloads the real TCB pointer from the TEB slot, dereferences the decoded TCB offset, performs the original operation, and jumps back. `CMP`/`XOR` scratch storage lives below the SysV red zone and cleanup uses flag-preserving instructions, so the original condition codes reach following guest instructions. The current patch arena is one host page per module; exhaustion and operand forms that cannot be preserved safely are load-time errors rather than silent approximations.

POSIX x86-64 native execution installs process-level SIGSEGV/SIGBUS/SIGILL handlers once, while Windows x64 installs a vectored exception handler. Capture state is thread-local and only active around native guest invocation. Both paths record the fault address and instruction pointer, rewrite the native exception context to the entry trampoline's saved host stack/recovery epilogue, and return through normal register restoration. POSIX faults outside an active guest invocation are chained to the previous handler; Windows returns `EXCEPTION_CONTINUE_SEARCH`. Intentional guest-thread termination reuses this transfer path: `pthread_exit` records the requested return value in the active capture and executes a controlled illegal-instruction trap. SIGILL/VEH recognizes that marked trap as termination rather than a guest fault and resumes at the same recovery epilogue. No C++ exception and no cross-stack `setjmp`/`longjmp` crosses guest or generated frames.

`GuestThread` composes an independent guest stack, TLS/TCB context, segment scope, entry trampoline, completion signal, and captured result on a real host thread. `GuestThreadManager` owns opaque guest thread handles per runtime and propagates both the manager/current handle and the runtime-owned `KernelServices` pointer through one thread-local scope, so `pthread_self` is stable from the first guest instruction, HLE services are available inside child guest workers, and a child can create another thread without a process-global runtime singleton. `pthread_create`, `pthread_join`, `pthread_timedjoin_np`, `pthread_self`, `pthread_detach`, and `pthread_exit` are registered for both `libkernel` and `libScePosix`. Joiners claim a record before waiting; a second joiner gets `ENOTSUP`, detach is rejected while claimed, and a timed-out absolute realtime wait releases the claim without removing the handle. Runtime-owned opaque thread-attribute handles model the ABI-relevant 1 MiB default/16 KiB minimum stack size and joinable/detached state. Guest detach marks the handle non-joinable but intentionally keeps the host `std::thread` runtime-owned. Finished detached records are reaped lazily, and manager shutdown prevents new creates before joining any remaining workers outside the manager lock.

The next native-execution work is:

- multi-page or demand-grown Windows CPU-patch arenas plus rarer TCB operand forms that cannot use the current scratch template;
- a cancellation-point/interruption layer plus cleanup handlers, stronger thread-state diagnostics, and scheduler-aware synchronization attributes;
- a narrow fallback mechanism for instructions or behaviors that cannot execute directly.

The CPU layer must not become a general DBT unless evidence shows it is necessary.

### Initial libkernel HLE

The HLE registry shares the same `SymbolKey` space as guest exports. `KernelServices` is runtime-owned rather than global and is reached through the existing guest-thread manager scope. The current slice covers process time/counter/frequency, current CPU, `sceKernelGetDirectMemorySize`, `sceKernelMprotect`, a deny-by-default read-only `/app0` file core (`open`/`read`/`close`/`lseek`/`stat`/`fstat`) exposed through both POSIX and kernel NIDs, pthread create/join/timed-join/self/detach/exit, stack-size/detach-state thread attributes, typed mutexes and bounded mutex attributes, clock-aware condition variables with absolute/relative timed waits, POSIX and `scePthread` semaphores, and nanosleep/usleep/sleep/yield calls. POSIX file/semaphore/sleep failures use a per-host-thread guest errno exposed through `__Error`; `scePthread*` and `sceKernel*` wrappers preserve the ORBIS `0x8002xxxx` convention.

`mprotect` uses 16 KiB guest-page alignment and can split one mapped guest region while preserving the unaffected subranges. The current direct-memory-size query reports the configured native guest arena capacity; NyxoraRT does not invent a console hardware constant before a direct/flexible memory allocator exists. A range that crosses separately tracked regions is not yet coalesced by this HLE.

The file slice is intentionally read-only. `/app0` has no implicit host root: an embedding host must configure it, while `Runtime::load_elf()` initializes it from the first executable directory when no root was supplied. Canonical-path checks keep `..` and symlink resolution inside that root, and write/create/truncate/append flags are rejected. Seek/stat and any writable mount policy remain future work.

Mutex objects are opaque guest handles backed by runtime-owned host records. Null/static mutex slots are lazily initialized as error-checking mutexes, while the adaptive static initializer retains the adaptive type. Runtime-owned mutex attributes expose the four public types (`ErrorCheck`, `Recursive`, `Normal`, `AdaptiveNp`) and process-private sharing. Recursive depth is tracked explicitly and condition waits save/release/restore that depth around the atomic wait transition. Error-check/adaptive self-locks report deadlock, normal self-lock intentionally blocks, and recursive self-lock increments depth. Priority inheritance/protect and priority ceilings are not registered because the runtime has no guest scheduler capable of enforcing them. Destroy refuses locked/waited objects. Condition variables use their own opaque records and per-waiter wake tokens: wait enqueues before releasing the owned mutex, then reacquires that mutex before return; signal wakes one queued waiter and broadcast wakes the full captured queue. Null condition slots lazily initialize, destroy refuses queued waiters, and all condition operations follow the table-then-record lock order so destroy cannot race between lookup and enqueue. Condition attributes currently model process-private state and the public realtime, virtual-process-CPU, profiling-process-CPU, and monotonic clocks. Absolute and relative timed waits reuse the same queue/reacquire path and resolve timeout-vs-signal races while holding the condition record.

POSIX/`scePthread` semaphores are also runtime-owned opaque records with a bounded count, wait/trywait/timed-wait/post/getvalue operations, and explicit waiter publication. Wait publication uses the same table-then-record ordering as destroy, so a guest-visible semaphore cannot be removed in the lookup-to-sleep gap. Concurrent destruction with published waiters is rejected as busy to avoid orphan host waits. The more general `sceKernelCreateSema` family remains unimplemented. Sleep HLE validates guest timespec memory through `GuestAddressSpace`; interruption is not yet modeled, so successful nanosleep completes the requested duration and reports zero remaining time.

Pthread cancellation and cleanup-handler exports remain deliberately unregistered. Correct cancellation requires pending/enable/type state per guest thread, cancellation-point integration with blocking HLE, cross-thread host interruption (signal/APC or an equivalent wake mechanism), join-target wakeups, and execution of guest cleanup callbacks before termination. Returning success without all of those pieces would create unsafe partial ABI compatibility, so the runtime first needs a dedicated cancellation/interruption layer.

On SysV x86-64 hosts HLE bindings can point directly at host implementations. Windows uses the generated bridge described above: it remaps the first four integer/pointer arguments and executes host C++ on the real OS stack. Signatures wider than four integer/pointer arguments still require an explicit bridge extension rather than unsafe casts.

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
