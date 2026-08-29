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
- decoded x86-64 TCB patching using pinned Zydis 4.1.1: Linux rewrites supported `FS:[TCB]` accesses to GS; Windows keeps `FS:[0]` as an in-place TEB-slot rewrite and sends supported nonzero `MOV`/`CMP`/`XOR` reads through near side thunks that reload the real TCB pointer; unsupported forms fail loading instead of running with incorrect host TLS semantics;
- callable late-import thunks backed by immutable RX code and separate RW target/counter slots; unresolved calls are counted and later bindings update stale thunk pointers without rewriting code;
- a first `libkernel` HLE slice for process-time counters/frequency and current-CPU queries, registered through the same version-aware symbol path as guest exports;
- `Runtime::invoke_entry()` as a synchronous end-to-end path from a native-backed loaded module through guest stack/thread context to native entry execution;
- POSIX guest fault capture for SIGSEGV/SIGBUS/SIGILL and Windows x64 vectored-exception recovery, both returning through the native entry recovery epilogue;
- `GuestThread`/`Runtime::start_thread()` plus a runtime-owned thread manager, with `pthread_create`, `pthread_join`, `pthread_self`, and guest-semantic `pthread_detach` HLE for `libkernel` and `libScePosix`; detached guest threads remain host-owned until completion/shutdown rather than escaping the runtime;
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

1. Grow Windows CPU-patch capacity beyond the current one-page near arena and cover rarer TCB operand forms; add pthread attributes and a dedicated safe guest-thread termination primitive before exposing `pthread_exit`.
2. Expand `libkernel` HLE into memory, files, synchronization, and sleep/time.
3. Module lifecycle: dependency loading, init/fini arrays, process parameters, and richer runtime-owned thread tracking.
4. PM4 command processor with deterministic tracing and state tracking.
5. Shader frontend -> typed IR -> SPIR-V backend and persistent shader/pipeline cache.
6. Vulkan resource tracking, synchronization, presentation, and performance tooling.

See [`docs/architecture.md`](docs/architecture.md) and [`docs/research-notes.md`](docs/research-notes.md).

## Scope and legal boundary

NyxoraRT is intended for interoperability research, homebrew, and legally obtained/decrypted test inputs. The project will not include console keys, DRM bypasses, copyrighted firmware, or game assets.

## License

Apache-2.0. See [`LICENSE`](LICENSE).
