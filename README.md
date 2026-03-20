<p align="center">
  <img src="resources/Icon.png" alt="WhiteoutTex" width="128" />
</p>

<h1 align="center">WhiteoutTex</h1>

<p align="center">
  A fast, lightweight texture viewer and converter for game assets — with optional AI upscaling.
</p>

<p align="center">
  <img src="resources/media/media_0.png" alt="Image Viewer" width="80%" />
</p>

<details>
<summary><h3 align="center">🖼️ More Screenshots</h3></summary>

<p align="center">
  <img src="resources/media/media_1.png" alt="Texture Details" width="80%" />
</p>
<br />
<p align="center">
  <img src="resources/media/media_2.png" alt="Batch Conversion" width="80%" />
</p>
<br />
<p align="center">
  <img src="resources/media/media_3.png" alt="CASC Browser" width="80%" />
</p>
<br />
<p align="center">
  <img src="resources/media/media_4.png" alt="Save Options" width="80%" />
</p>
<br />
<p align="center">
  <img src="resources/media/media_5.png" alt="AI Upscaling" width="80%" />
</p>

</details>

---

## Features

- **Open & save** textures: BLP, BMP, DDS, JPEG, PNG, TGA — plus read-only support for Blizzard TEX (Diablo III/IV).
- **Image viewer** with zoom, pan, per-channel RGBA filtering, mip level selection, and auto-detection of normal maps and ORM textures.
- **Texture details** — dimensions, pixel format, mip chain info, and automatic texture kind classification (Diffuse, Normal, ORM, etc.) with manual override.
- **Mipmap generation** — one-click regeneration that respects texture kind (normal maps get renormalized, PBR textures keep correct properties).
- **Batch conversion** — convert entire directories at once, multi-threaded, with per-format filters and per-kind DDS targeting.
- **CASC browser** — open Blizzard game archives directly and browse/extract textures, including full Diablo IV TEX support.
- **Save options** — format-specific settings (BLP encoding & dithering, DDS format, JPEG quality), mipmap generation on save, and preferences remembered across sessions.

### AI Upscaling (Optional)

GPU-accelerated texture upscaling powered by **Real-ESRGAN** via Vulkan compute. Available models:

- **realesrgan-x4plus** — general-purpose 4× upscale
- **realesrgan-x4plus-anime** — anime/stylized 4× upscale
- **realesr-animevideov3** — 2×, 3×, or 4× upscale

Upscaling can be applied to individual textures or as part of a batch conversion pipeline. Requires building with `WHITEOUT_ENABLE_UPSCALER=ON` and a Vulkan SDK.

---

## Building

Requires **CMake** and a **C++23** compiler.

```bash
cmake -S . -B build
cmake --build build --config Release
```

For AI upscaling support (optional):

```bash
cmake -S . -B build -DWHITEOUT_ENABLE_UPSCALER=ON
cmake --build build --config Release
```

This requires the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home). Run `scripts/download_models.ps1` to fetch the model files.

---

## License

**BSD 3-Clause** — see [LICENSE.md](LICENSE.md). An [AI-Derived Works Notice](LICENSE-AI.md) applies to AI-generated code based on this software.

## Third-Party Notices

| Library | License |
|---------|---------|
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [SDL3](https://github.com/libsdl-org/SDL) | zlib |
| [WhiteoutLib](https://github.com/FernandoS27/WhiteoutLib) | See library license |
| [CascLib](https://github.com/ladislav-zezula/CascLib) | MIT |
| [Real-ESRGAN ncnn](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan) | MIT *(optional)* |

Full texts in [THIRD_PARTY.md](THIRD_PARTY.md).

## Disclaimer

This project is not affiliated with or endorsed by Blizzard Entertainment.
