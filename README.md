# NyxoraRT

NyxoraRT is an experimental high-performance **x86-64 console user-mode compatibility runtime**. Its core design goal is to avoid full CPU emulation where the guest and host ISA already match, while translating operating-system ABI and GPU behavior into portable host implementations.

The project is intentionally early. It does **not** currently run commercial console games.

## Design direction

- **Native-first CPU path**: preserve guest x86-64 code and build the runtime around ABI thunks rather than an always-on interpreter.
- **Explicit HLE boundary**: imports are resolved through a version-aware symbol registry that can target HLE functions or guest exports.
- **Guest address-space model**: the same memory policy can use deterministic buffered storage or an identity-mapped native arena, so linker behavior does not fork between tests and direct execution.
- **GPU command boundary**: guest command streams enter through a small backend interface. The first real backend is planned around PM4 decoding, an internal state model, shader IR, and Vulkan.
- **Per-title compatibility without contaminating the core**: quirks and patches will live above the generic runtime rather than inside low-level primitives.

## Current milestone

The initial framework already provides:

- bounds-checked 64-bit x86 ELF parsing, including the SCE dynamic executable types and key program-header types;
- load-segment mapping and BSS initialization into a guest address-space abstraction;
- SCE dynamic metadata parsing for string/symbol tables, module/library identities, RELA/PLT relocations, and PT_TLS metadata;
- a version-aware NID/library/module symbol registry for HLE functions and loaded guest exports;
- a runtime linker for `R_X86_64_64`, `GLOB_DAT`, `JUMP_SLOT`, `RELATIVE`, and `DTPMOD64`, including retryable unresolved imports;
- buffered and identity-mapped native guest memory with W^X transitions during loading/relocation;
- guarded guest stacks plus an x86-64 entry trampoline that switches `RSP`/`RBP` and presents SysV guest arguments on Linux, macOS x86-64, and Windows x64;
- per-thread PT_TLS images with original alignment, initialized bytes, zeroed TLS BSS, a 0x40-byte guest TCB/DTV model, and scoped runtime thread context;
- Linux x86-64 guest TCB binding through GS, with restoration on exit; FS-based guest TCB instructions still require an explicit FS->GS rewrite stage before unmodified binaries can use this path;
- callable late-import thunks backed by immutable RX code and separate RW target/counter slots; unresolved calls are counted and later bindings update stale thunk pointers without rewriting code;
- a first `libkernel` HLE slice for process-time counters/frequency and current-CPU queries, registered through the same version-aware symbol path as guest exports;
- `Runtime::invoke_entry()` as a synchronous end-to-end path from a native-backed loaded module through guest stack/thread context to native entry execution;
- POSIX guest fault capture for SIGSEGV/SIGBUS/SIGILL with host-handler chaining, plus `GuestThread`/`Runtime::start_thread()` for independent guest stack/TLS/TCB execution and joinable results;
- a bounds-checked PM4 packet frontend, GPU submission/timeline interface, and deterministic null backend;
- unit tests and CI-ready CMake/CTest targets.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

To inspect a legal, unencrypted x86-64 ELF test image:

```bash
./build/nyxora ./path/to/test.elf
```

## Near-term roadmap

1. Safe FS-TCB instruction rewriting to the host-specific guest-TCB path; add Windows TCB access rewriting and VEH-backed guest fault capture.
2. Expand `libkernel` HLE into memory, files, synchronization, sleep/time, and map pthread APIs onto the new `GuestThread` lifecycle.
3. Module lifecycle: dependency loading, init/fini arrays, process parameters, and runtime-owned guest-thread tracking.
4. PM4 command processor with deterministic tracing and state tracking.
5. Shader frontend -> typed IR -> SPIR-V backend and persistent shader/pipeline cache.
6. Vulkan resource tracking, synchronization, presentation, and performance tooling.

See [`docs/architecture.md`](docs/architecture.md) and [`docs/research-notes.md`](docs/research-notes.md).

## Scope and legal boundary

NyxoraRT is intended for interoperability research, homebrew, and legally obtained/decrypted test inputs. The project will not include console keys, DRM bypasses, copyrighted firmware, or game assets.

## License

Apache-2.0. See [`LICENSE`](LICENSE).
