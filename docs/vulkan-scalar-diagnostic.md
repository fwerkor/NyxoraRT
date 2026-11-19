# Headless Vulkan scalar diagnostic execution

NyxoraRT now has an **optional** Vulkan compute harness that runs the
restricted SPIR-V output emitted by `--shader-spv`. It is off by default
and introduces no mandatory Vulkan dependency into the emulator runtime.

## Build

Install Khronos Vulkan headers/loader and a Vulkan ICD. On Ubuntu, Lavapipe
is supplied by `mesa-vulkan-drivers`. Then:

```bash
cmake -S . -B build-vulkan \
  -DNYXORA_ENABLE_VULKAN_DIAGNOSTICS=ON \
  -DNYXORA_ENABLE_WERROR=ON
cmake --build build-vulkan --parallel

./build-vulkan/nyxora --shader-spv ./shader.bin ./shader.spv
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build-vulkan/nyxora_vulkan_scalar ./shader.spv
```

The command outputs `{"sgpr":[...],"scc":0}`, containing the **102 SGPR
registers** and SCC as observed after a real Vulkan compute dispatch. The
device buffer is initialized to zero for this diagnostic mode. The runner
uses one descriptor set, one 103-element host-coherent storage buffer,
one compute pipeline, one workgroup of one invocation and a shader-write
to host-read memory barrier.

The 32-program conformance suite can also test actual Vulkan execution:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ctest --test-dir build-vulkan \
  -R '^nyxora_vulkan_scalar_conformance$' --output-on-failure
```

The suite executes **32 deterministic generated compute shaders** and
compares the returned 103 values to an independent Python scalar reference.
The same program corpus additionally checks NyxoraRT's CPU interpreter
and Khronos `spirv-val`. On the development host, all 32 produced matching
results on Mesa Lavapipe.

GitHub Actions includes a separate Ubuntu `vulkan-diagnostic` job that
builds the optional executable and runs this test on Lavapipe, while the
existing Linux/Windows/macOS core build matrix remains SDK-independent.

## Scope and safety boundary

The utility is an interoperability/verification harness, **not** a
general Vulkan graphics backend or console GPU scheduler. The accepted
shader subset is limited to straight-line scalar SGPR/SCC arithmetic;
no actual PS5 command buffer, graphics queue, swapchain, texture, wave
resource, VCC/EXEC branch, synchronization, or game rendering is modeled.

The diagnostic buffer ABI (set 0, binding 0; 103 uint32 elements) is not
the console's hardware register representation. The tool executes
user-supplied SPIR-V in the Vulkan driver: only use binaries from
trusted inputs. Driver behavior and hardware performance may vary.

No proprietary shader binaries, game assets, firmware or console keys
are included.
