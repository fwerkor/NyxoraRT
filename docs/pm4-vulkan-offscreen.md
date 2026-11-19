# First PM4-to-Vulkan offscreen graphics diagnostic

This optional test path proves the smallest **command-to-pixel** chain:

```text
Headerless PM4 packet -> transactional GCN PM4 decoder
 -> typed DRAW_INDEX_AUTO (3 vertices) -> Vulkan graphics pipeline
 -> offscreen RGBA8 render pass -> image/buffer barriers
 -> host-visible pixel copy -> 64×64 PPM image
```

It is a **fixed-shader test harness**. The vertex and fragment shaders
are small, originally written SPIR-V assembly files under
`tests/shaders/`. Their test content is built using Khronos
`spirv-as` and is independent of real guest shaders.

## Build and reproduce

Requires the Vulkan loader/headers, a graphics-capable Vulkan ICD,
Khronos `spirv-as`, and Python for the conformance test.

```bash
cmake -S . -B build-vulkan -DNYXORA_ENABLE_VULKAN_DIAGNOSTICS=ON
cmake --build build-vulkan --parallel

VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ctest --test-dir build-vulkan \
  -R '^nyxora_vulkan_triangle_conformance$' --output-on-failure
```

To run the executable directly, make a raw little-endian PM4 file with
exactly one `DRAW_INDEX_AUTO` Type-3 packet:

```python
import struct
with open("triangle.pm4", "wb") as f:
    f.write(struct.pack("<3I", 0xC0012D00, 3, 0))
```

```bash
./build-vulkan/nyxora_vulkan_triangle triangle.pm4 \
  build-vulkan/triangle.vert.spv build-vulkan/triangle.frag.spv triangle.ppm
```

The output is a binary PPM P6 image. On Mesa Lavapipe, the center
pixel is red (255,0,0), the four corners are black, and 1152 of
4096 pixels are red. The test accepts small rasterization-edge
differences across devices. It also checks five intentionally invalid
PM4 traces fail without producing an image.

## Implementation boundaries

- Strictly accepts a **single** normalized `DRAW_INDEX_AUTO` call,
  exactly three vertices and no shader-register state or additional
  PM4 commands. `DRAW_INDEX_2`, hardware index buffers, multiple
  draws and actual primitive assembly are not yet supported.
- The test shaders emit triangle positions using `VertexIndex` and
  write a fixed red fragment color. There is **no guest vertex/pixel
  shader translation** or guest VRAM resource binding.
- Uses a Vulkan RGBA8 color attachment, fixed pipeline state, one
  render pass and direct buffer readback. It does **not** implement
  swapchains, frames-in-flight, guest resource coherency, render-target
  state, dynamic shaders, fences/timelines or presentation.
- This is a development diagnostic. It must **not** be interpreted as
  proof that PS5 games, including GTA6, can render.
- Input shader binaries are executed by the host graphics driver; test
  only known/trusted SPIR-V modules.

GitHub Actions runs the rasterization test on Mesa Lavapipe alongside
existing scalar and vector diagnostic compute conformance. The default
SDK-free CMake targets are unaffected.
