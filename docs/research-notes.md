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

## Implemented PM4 frontend milestone

The first command-processor milestone uses public AMD/Linux PM4 definitions only to verify packet header fields, Type3 opcode numbers, register-window bases, and packet payload sizes. NyxoraRT independently implements the state machine. The shared frontend currently normalizes Type0 writes, `SET_CONFIG_REG`, `SET_CONTEXT_REG`, `SET_SH_REG`, `SET_UCONFIG_REG`, `NUM_INSTANCES`, `DRAW_INDEX_AUTO`, and `DISPATCH_DIRECT`. Graphics and compute SH state are kept separate, invalid submissions are transactional, and unsupported stateful packets fail rather than being ignored.

The host backend boundary now sits after this normalization step. The null backend consumes typed commands and records semantic submission statistics, while future Vulkan code can consume the same ordered register/draw/dispatch stream without re-decoding PM4. Predication, indirect execution, memory writes, events/barriers, and reset/default-state packets remain deliberately unsupported because their effects cannot yet be represented faithfully.

## Implemented runtime-linker milestone

The first implementation derived only format and architectural facts from the references, then reimplemented the behavior independently. NyxoraRT now parses SCE dynamic-table offsets from `PT_SCE_DYNLIBDATA`, decodes packed module/library identifiers, models PT_TLS metadata, applies the core x86-64 RELA forms used by the references, and keeps unresolved imports retryable.

The memory layer now has an identity-mapped native arena in addition to deterministic buffered storage. The same runtime-linker tests run against the shared memory policy, and an end-to-end synthetic SCE fixture verifies load -> relocate -> W^X protection -> native entry execution without introducing a CPU interpreter.

## Guest-thread and late-import milestone

Further study of the public reference implementations reinforced three design choices. First, guest-stack switching is a runtime concern independent of ELF parsing. Second, x86-64 console TLS should not be modeled by overwriting the host process TLS model; NyxoraRT now keeps explicit per-thread PT_TLS images while segment-base binding remains a separate next step. Third, unresolved callable imports benefit from stable tail-jump thunks whose writable target state is separated from executable code.

NyxoraRT now has its own generated x86-64 entry trampoline, guarded guest stacks, per-thread TLS images, stable late-import thunks, and an initial `libkernel` HLE registry. Reference projects were used to establish public ABI/format facts and NID/version metadata; the implementation remains independent and no GPL source is incorporated.

## Zydis

NyxoraRT uses upstream Zydis 4.1.1 (MIT) as the instruction-decoding boundary for CPU compatibility patches. The dependency is pinned to an exact upstream revision. It is used to establish instruction boundaries and operand semantics before modifying segment prefixes; NyxoraRT does not incorporate GPL patching code from the emulator references.

## Windows nonzero TCB side-thunk milestone

Windows x64 can now execute the supported nonzero `FS:[TCB offset]` read forms without mirroring TCB state. Decoded `MOV`, `CMP`, and `XOR` sites jump to a nearby runtime-owned thunk that reads the actual per-thread TCB pointer from the Win32 TLS slot, performs the offset access, and returns to the next guest instruction. The `FS:[0]` case remains an in-place TEB-slot rewrite. This keeps mutable TCB fields single-sourced and makes unsupported operand shapes or patch-arena exhaustion explicit load failures.

## Core libkernel services milestone

The first stateful HLE services sit behind a runtime-owned `KernelServices` object rather than process-global emulator state. Public reference source was used only to confirm function signatures, NIDs, protection constants, and POSIX-versus-ORBIS error conventions. NyxoraRT independently implements bounded guest protection changes, deny-by-default read-only `/app0` file access, and runtime-owned pthread synchronization objects. Write-capable mounts and synchronization families whose scheduler/lifecycle semantics are not modeled remain absent instead of being registered as no-op success stubs.

Windows HLE execution was strengthened at the same milestone. The entry trampoline records the suspended OS stack in a dedicated inline Win32 TLS slot, and generated SysV-to-MS-x64 HLE bridges switch back to that stack before entering C++ host code. The previous slot value is stored in the entry frame and restored on both ordinary and vectored-exception recovery paths. This keeps filesystem/synchronization implementations away from the guest stack and its incompatible Windows TEB stack bounds.

## Pthread termination and synchronization milestone

Public reference code was used only to verify pthread NIDs, default/minimum stack sizes, detach-state values, condition static/destroyed sentinels, and POSIX-versus-ORBIS return conventions. NyxoraRT independently added three bounded runtime mechanisms. First, `pthread_exit` records its return value in the active native invocation and deliberately traps into the existing SIGILL/VEH recovery epilogue, so a guest thread can terminate without C++ unwinding or cross-stack `longjmp`. Second, runtime-owned pthread-attribute handles expose only stack size and detach state; `pthread_create` consumes those values directly. Third, condition variables use opaque runtime records and explicit per-waiter wake tokens, preserving enqueue-before-mutex-release ordering and mutex reacquisition before `pthread_cond_wait` returns.

The condition implementation keeps table lookup and record locking in one order for wait/signal/broadcast/destroy, preventing a destroy from removing the guest-visible object between lookup and waiter enqueue. Two-waiter broadcast and end-to-end `pthread_exit` through `GuestThreadManager::join` are covered by tests.

## Timed synchronization and sleep milestone

Public reference source was used to verify condition-clock IDs, timed-wait and condition-attribute NIDs, POSIX semaphore NIDs/error values, `scePthreadSem*` wrappers, `__Error`, and sleep/yield exports. NyxoraRT independently extends the runtime-owned synchronization model with condition attributes for realtime, virtual process CPU, profiling process CPU, and monotonic clocks; absolute and relative condition timeouts; and POSIX/`scePthread` semaphores with bounded counts, try/timed waits, overflow handling, and getvalue. Timeout paths preserve the existing mutex reacquisition guarantee and resolve signal races before reporting `ETIMEDOUT`.

POSIX semaphore and sleep APIs now have a host-thread-local guest errno cell exposed through `__Error`, so `-1` returns carry the corresponding guest errno instead of losing half of the ABI contract. Semaphore wait publication follows table-to-record lock ordering before the host wait begins; this closes the same destroy-between-lookup-and-enqueue class previously found in condition variables. The general `sceKernelCreateSema` counting/wait-count interface is still absent because it has different semantics from POSIX semaphores. Nanosleep/usleep/sleep and pthread/scheduler yield are mapped without signal interruption yet; nanosleep validates request/remaining memory through the guest address space and reports zero remaining time after uninterrupted completion.

## Timed join and mutex attributes milestone

Public reference source was used to verify the `pthread_timedjoin_np` NID, absolute realtime deadline contract, second-joiner behavior, mutex type values, process-sharing behavior, and mutex-attribute NIDs. NyxoraRT independently adds a completion condition to guest threads and a join-claim state to the runtime-owned thread record. A timed join validates guest `timespec` memory through `KernelServices`, waits without polling, returns `ETIMEDOUT` without consuming the handle, rejects a second concurrent joiner with `ENOTSUP`, and prevents detach while a join claim is active.

Mutex attributes now model the observable type/pshared subset: error-checking, recursive, normal, and adaptive types plus process-private sharing. Recursive depth is part of the runtime mutex record and is preserved across condition waits. Adaptive currently differs from error-checking only in its guest-visible type/self-lock behavior; host spin/yield tuning APIs are not registered because they are performance hints rather than correctness requirements. Priority inheritance/protect and ceiling attributes remain absent because NyxoraRT has no guest priority scheduler to enforce them.

Cancellation and cleanup-handler NIDs were studied but deliberately left unregistered. The public contract requires cross-thread interruption, deferred/asynchronous cancellation state, cancellation-point wakeups (including a target blocked in join), and execution of guest cleanup callbacks before the canceled return value becomes observable. The current fault/termination route handles the running thread only and is not a safe substitute for those mechanisms.

## Read-only file metadata milestone

Public reference source was used only to verify file-system NIDs, `lseek` whence/error conventions, and the 120-byte guest stat layout. NyxoraRT independently extends its existing deny-by-default `/app0` sandbox with POSIX/raw aliases for read-only open/read/close and adds lseek/stat/fstat. The file table remains runtime-owned; each open regular file retains its canonical sandbox path, and stat/fstat write only verified guest fields. Host inode, device, uid/gid, and other platform-specific identities are deliberately zeroed. Guest modification time uses a deterministic platform conversion (Win32 FILETIME on Windows and the C++20 file clock elsewhere), so repeated stat/fstat calls are stable.

Cancellation was reconsidered during this milestone but remains intentionally absent. The current join, condition, semaphore, and sleep paths use separate host waits; adding cancellation state without a common interruption/wakeup layer would leave blocked cancellation points uninterruptible and would still omit guest cleanup callback execution. No cancellation or cleanup NIDs were registered.

## Memory mapping milestone

Public reference source was used only to verify the guest 16 KiB page contract, mmap/munmap/mprotect NIDs and signatures, map/protection flag values, alignment/error behavior, and the observed read-only file-map permission rules. NyxoraRT independently adds subrange unmapping and a bounded virtual-memory service. Anonymous, stack, and reserved mappings are represented directly in `GuestAddressSpace`; `MAP_FIXED` replacement and no-overwrite are explicit. Read-only regular-file `MAP_PRIVATE` uses a snapshot from the already-open file object and restores its logical stream position, then drops write/execute permission and zero-fills the aligned tail. Shared file mappings, GPU-protected mappings, and anonymous system-memory mappings return `ENOTSUP` because coherence, GPU address-space, and pool-accounting semantics are not modeled.

The same work exposed an ABI prerequisite on Windows: the existing HLE bridge only forwarded four SysV arguments. The bridge now preserves guest nonvolatile registers, saves arguments 4 and 5 from R8/R9 plus argument 6 from the guest stack, switches to the saved Windows host stack, lays those values out after Microsoft x64 shadow space, and calls the host target. A seven-argument generated guest call test covers the bridge independently of mmap.

## Virtual-memory accounting and query milestone

Public reference source was used only to verify the 16 KiB guest page contract, the 72-byte `VirtualQueryInfo` and 24-byte direct-query layouts, query flags, memory NIDs/signatures, map/protection flags, alignment rules, and observable error conventions. The implementation was written independently. `GuestAddressSpace::RegionInfo` now carries semantic mapping kind, offset, memory type, commitment state, and auxiliary GPU protection bits, and those fields are preserved when protection or unmap operations split a region. `sceKernelVirtualQuery` and `sceKernelQueryMemoryProtection` therefore read the runtime's existing mapping state rather than maintaining a parallel VMA database.

`KernelServices` now partitions the 16 KiB-aligned native managed span into a direct region and an optional host-configured flexible region. Flexible capacity must be configured before guest mappings exist; map/unmap updates available capacity, partial unmap returns only the released bytes, and reused flexible pages are zero-filled. Direct allocation tracks physical offset intervals, honors zero/default and arbitrary 16 KiB-multiple alignment, reuses released holes, supports partial and checked release, and coalesces adjacent same-type ranges for direct-memory queries. Reallocation zeroes the canonical backing, while ordinary unmap/remap preserves it.

The direct-memory implementation intentionally supports only a canonical mapping, `guest_va = managed_base + physical_offset`. Because the current `NativeArena` is one guest virtual reservation rather than a separately aliasable physical backing object, arbitrary aliases return `ENOTSUP` and ordinary mappings occupying a canonical direct VA make that physical interval unavailable. This is a bounded compatibility subset, not an emulation of hardware aliasing. Shared backing/alias mappings, system-flexible memory, memory pools, batch mapping, PRT state, and broader GPU memory semantics remain future VM work.

The guest ABI path is covered end to end, including the seven-argument named direct-memory call through the Windows SysV-to-MS-x64 bridge. Tests also pin the verified virtual-query field offsets, exact query structure sizes, direct backing persistence across unmap/remap, zeroing after release/reallocation, physical-hole reuse, flexible partial-unmap accounting, `EAGAIN` allocation failure, checked-release `ENOENT`, and named-map `EFAULT`/`ENAMETOOLONG` behavior.

## Dependency and module-lifecycle milestone

Public ELF ABI material was used to confirm `DT_NEEDED`, `DT_RPATH`, and `DT_RUNPATH` metadata plus constructor/destructor ordering, while public SCE metadata examples were used only to confirm that filesystem dependency names coexist with SCE module/library identities. NyxoraRT independently implements a bounded same-root dependency graph: canonical paths are deduplicated before recursion, cycles are accepted, path traversal/symlink escape is rejected, and missing `libkernel.prx`/`libScePosix.prx` files can be satisfied by the runtime's existing HLE registrations. Bare dependency names first retain same-directory lookup, then use `DT_RUNPATH` in preference to `DT_RPATH`; `$ORIGIN`/`${ORIGIN}` and guest `/app0` are expanded without exposing the host working directory or arbitrary absolute host paths. Discovery completes before guest mapping begins, then every guest export is registered before the relocation pass so dependency cycles can resolve against already-visible peers.

Lifecycle execution uses one guest stack/trampoline/TLS context per phase. The executable preinit array runs once; graph dependencies initialize before their dependents; `DT_INIT` precedes the init array; finalization uses reverse completed-initialization order, reverses each fini array, then invokes `DT_FINI`. Module completion is committed only after its full initializer sequence returns, allowing a failed dependent constructor to leave completed dependencies available for deterministic cleanup. Tests execute native x86-64 synthetic constructors/destructors that append to a shared trace and pin both normal and partial-failure order.

`PT_SCE_PROCPARAM` is exposed only as a mapped guest range after validating that the advertised span lies inside a loaded segment. NyxoraRT does not yet parse version-specific process-parameter internals. Dynamic search paths remain deliberately bounded: empty path components do not import host-CWD semantics, unknown `$...` substitutions are rejected, `RPATH` is not yet inherited through dependency ancestry, and firmware/system namespaces, runtime unload, and TLS deregistration remain future work.
