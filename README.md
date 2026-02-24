# RitoTex — Photoshop Texture Plugin for League of Legends Modding

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](license.txt)
[![Platform](https://img.shields.io/badge/Platform-Windows%2064--bit-lightgrey.svg)]()
[![Photoshop](https://img.shields.io/badge/Photoshop-CS6%2B-blue.svg)]()

A Photoshop plugin for the League of Legends modding community. Load, edit, and save `.tex` and `.dds` textures directly in Photoshop with BC1/BC3 compression and optional error-diffusion dithering.

Based on Intel® Texture Works (discontinued).

---

## Features

- **TEX format** — Load and save League's `.tex` files (DXT1/DXT5, mipmaps)
- **DDS format** — Full DDS support with BC1–BC7 compression
- **Error diffusion dithering** — Floyd-Steinberg dithering for BC1/BC3 to reduce banding
- **Auto format detection** — Save as `.tex` or `.dds` based on file extension
- **Mipmap generation** — Auto-generate or build from Photoshop layers
- **Preview window** — Compare compression quality before saving
- **Preset system** — Save and load compression settings
- **ISPC compressor** — Fast, high-quality texture compression

---

## Installation

1. **Download** `RitoTex.8bi` from [Releases](https://github.com/RitoShark/Photoshop-Plugin/releases) or build from source
2. **Copy** to your Photoshop plugins folder:
   ```
   C:\Program Files\Adobe\Adobe Photoshop [version]\Plug-ins\File Formats\
   ```
3. **Restart Photoshop**

---

## Usage

### Save a texture
1. **File → Save As** → set format to **RitoTex**
2. Enter filename with `.tex` or `.dds` extension — format is auto-detected
3. Choose compression (BC1 for RGB, BC3 for RGBA), mipmap mode, and dithering
4. Click **OK**

### Open a texture
1. **File → Open** (or **Open As** → select **RitoTex**)
2. Select a `.tex` or `.dds` file

### Dithering
Enable **Error Diffusion Dithering** in the save dialog to reduce color banding in BC1/BC3 textures. Choose between **Perceptual** and **Uniform** error metrics.

---

## TEX Format Reference

```
Header (12 bytes):
  magic[4]      "TEX\0"
  width          uint16
  height         uint16
  unk1           uint8  (0)
  format         uint8  (0x0A=DXT1, 0x0C=DXT5)
  unk2           uint8  (0)
  has_mipmaps    bool

Data: mipmaps stored smallest-to-largest, then main image
```

---

## Building from Source

### Requirements
- Visual Studio 2022 (v143 toolset)
- Windows 10 SDK
- Photoshop CS6 SDK
- Intel ISPC compiler (`3rdParty/Intel/Tools/ispc.exe`)

### Build
```powershell
.\build.ps1
```
Output: `Plugins\x64\RitoTex.8bi`

---

## License

Apache License 2.0 — see [license.txt](license.txt).

Based on [Intel® Texture Works Plugin](https://github.com/GameTechDev/Intel-Texture-Works-Plugin) (© 2017 Intel Corporation, Apache 2.0).

---

**Made for the League of Legends modding community by [RitoShark](https://github.com/RitoShark)**
