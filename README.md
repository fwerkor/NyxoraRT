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
- direct execution of a linked synthetic x86-64 SCE entry point in the test suite;
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

1. Guest-stack entry trampoline, per-thread TLS/FS context, and fault routing.
2. Callable late-import thunks so missing functions can be diagnosed at first use rather than only during relinking.
3. `libkernel` HLE substrate: memory, files, synchronization, time, and threads.
4. PM4 command processor with deterministic tracing and state tracking.
5. Shader frontend -> typed IR -> SPIR-V backend and persistent shader/pipeline cache.
6. Vulkan resource tracking, synchronization, presentation, and performance tooling.

See [`docs/architecture.md`](docs/architecture.md) and [`docs/research-notes.md`](docs/research-notes.md).

## Scope and legal boundary

NyxoraRT is intended for interoperability research, homebrew, and legally obtained/decrypted test inputs. The project will not include console keys, DRM bypasses, copyrighted firmware, or game assets.

## License

Apache-2.0. See [`LICENSE`](LICENSE).
