# RitoTex — Architecture

This document describes how the RitoTex Photoshop plug-in is structured after the
modular refactor. The goal of the layout is simple: **one responsibility per
file**, with the Photoshop-specific "host" code cleanly separated from the
host-agnostic format/codec code.

`IntelPlugin.cpp` used to be a ~3,500-line monolith. It is now a thin core: the
plug-in class lifecycle plus the `PluginMain` selector dispatch. Everything else
lives in a focused module.

---

## Layered view

```
                         Photoshop host
                               │  (format selectors)
                               ▼
        ┌─────────────────────────────────────────────┐
        │  IntelPlugin.cpp   — class core + PluginMain  │   HOST LAYER
        │                      selector dispatch         │  (talks to PS,
        ├───────────────┬───────────────┬───────────────┤   ps.formatRecord,
        │ PhotoshopRead │ PhotoshopWrite│   Preview      │   suites, dialogs)
        │  (load/parse) │ (save routing)│ (dialog render)│
        │               │   Scripting   │                │
        └───────┬───────┴───────┬───────┴───────────────┘
                │               │
                ▼               ▼
        ┌─────────────────────────────────────────────┐
        │  Codec  ·  ContainerWrite  ·  TexFormat       │   FORMAT / CODEC
        │  CubeMap · NormalMap · LayerOps · PixelConvert │   CORE
        │  s3tc                                          │  (no PS knowledge*)
        └───────────────────┬───────────────────────────┘
                            ▼
                  DirectXTex   +   Intel ISPC
```

\* The core operates on `DirectX::ScratchImage` / `rgba_surface` and config flags.
Some functions still read `ps.data->…` config and call `UserError`, since the
refactor preserved exact behavior rather than rewriting signatures — but they
contain no Photoshop file-I/O or selector logic.

---

## Modules

### Host layer (Photoshop-facing)

| File | Responsibility |
|------|----------------|
| **IntelPlugin.cpp / .h** | The `IntelPlugin` class: construction, `GetInstance` singleton, handle lifecycle (`CreateDataHandle`/`LockHandles`/`UnlockHandles`), `InitData`, `FetchImageData`/`DisposeImageData`, `SetProgress`, `UserError`, `CopyDataForEncoding` (PS buffer → uncompressed scratch), and **`PluginMain`** — the selector switch that routes Photoshop's read/write/options/estimate calls to the right handler. Also where the async update check is kicked off (`ReadStart`/`WriteStart`). |
| **PhotoshopRead.cpp** | The load/parse path: `DoReadPrepare/Start/Continue/Finish/LayerStart`, `DoFilterFile`, and `FillFromCompositedLayers`. Sniffs the file — `"TEX"` magic → `.tex` path (header parse, BC1/BC3 CPU decode via `s3tc`, RGBA repack); otherwise DDS via DirectXTex (`LoadFromDDSMemory`, decompress/convert, cube-face + mip-as-layer handling). Owns the file-scope `texLoadInfo` state. |
| **PhotoshopWrite.cpp** | Save orchestration: `DoWritePrepare/Start/Continue/Finish`. `DoWriteStart` shows the save dialog, then `IsSavingToTEXFormat` (sniffs `finalSpec`'s extension) routes to the TEX or DDS writer. |
| **Preview.cpp** | Everything the save dialog needs: reported dimensions/byte sizes, `FetchPreviewRGB` (blit into the dialog buffer), and the two scratch builders — `GetCompressedImageForPreview` (full encode → decode-back so the user sees real artifacts) and `GetUncompressedImageForPreview`. |
| **Scripting.cpp** | Actions/automation descriptor I/O: `Read/WriteScriptParamsForWrite`, `Read/WriteScriptParamsForRead`. Lets the plug-in participate in recorded actions and batch. |

### Format / codec core (host-agnostic)

| File | Responsibility |
|------|----------------|
| **TexFormat.cpp / .h** | Single source of truth for "what a `.tex` file looks like": the on-disk format codes (`tex_format`), the 12-byte `TEX_HEADER`, block-size math, `get_num_mipmaps`, and the DXGI ↔ tex format maps (`GetTEXFormatFromDXGI`, `BuildTEXHeader`). Pure and stateless. |
| **Codec.cpp** | The **shared** block-compression core. `CompressToScratchImage` turns an uncompressed RGBA8 scratch (single / mip chain / 6-face cube) into a compressed scratch. BC1/BC3 (dither off) → Intel ISPC (`ISPC_compression`, `DoPaddingToMultiplesOf4`); everything else (BC5/BC7, or BC1/BC3 with dither) → DirectXTex `Compress()`. This is the one encode path used by **both** the DDS and TEX writers. |
| **ContainerWrite.cpp** | The two file writers: `DoWriteDDS` (DirectXTex `SaveToDDSMemory`) and `DoWriteTEX` + `WriteTEXFile`/`WriteTEXMipmaps` (12-byte header, then mips smallest→largest, then base image). Both share the same pre-encode pipeline (fetch → cube/layer assembly → normal-map flip → mip gen → normalize → `Codec`); only the container framing differs. |
| **CubeMap.cpp** | Cube-map assembly/disassembly: cross ↔ 6-face cube, layers → cube, and saving a single cube mip level back out. Face order `+X,-X,+Y,-Y,+Z,-Z`. |
| **NormalMap.cpp** | Normal-map post-processing: X/Y channel flip and full-mip-chain renormalization (8-bit UNORM + 16-bit float). |
| **LayerOps.cpp** | Reading Photoshop document layers into a scratch image: raw channel reads, white-fill for missing alpha, per-channel copy, and the "mipmaps from layers" mapping. |
| **PixelConvert.cpp** | Convert Photoshop's source buffer (8 / 16 / 32-bit) into packed RGBA8 for the encoders; zero-fill missing channels, force opaque alpha. |
| **s3tc.cpp / .h** | CPU decode of DXT1/DXT5 blocks — used by the `.tex` read path. |

### Support

| File | Responsibility |
|------|----------------|
| **UpdateChecker.cpp / .h** | Background GitHub-Releases update check over WinHTTP. Detached thread, throttled 24h (HKCU), link-only. Traces to the Windows debug channel (`[RitoTex/Update]`). See [UpdateChecking_Design.md](UpdateChecking_Design.md). |
| **IntelPluginName.h** | Name + version (single source of truth) and the GitHub owner/repo the update checker queries. |
| **IntelPlugin.r / .pipl** | The PiPL resource that registers `.tex` and `.dds` as Photoshop file formats. |
| **kernel.ispc** | Intel ISPC source for the BC encoder kernels. |
| **\*Dialog.cpp** (CustomSaveDialog, SaveOptionsDialog, PreviewDialog), **IntelPluginUIWin.cpp** | The Win32 save / options / preview dialog UI. |

---

## How a save flows

```
PS: WriteStart selector
  └─ IntelPlugin::PluginMain  →  PhotoshopWrite::DoWriteStart
        ├─ CustomSaveDialog (pick format / compression / mips)
        └─ IsSavingToTEXFormat(finalSpec) ?
              ├─ yes →  ContainerWrite::DoWriteTEX
              └─ no  →  ContainerWrite::DoWriteDDS
                    └─ both:  CopyDataForEncoding         (IntelPlugin)
                              → cube/layer assembly        (CubeMap / LayerOps)
                              → normal-map flip            (NormalMap)
                              → mip generation             (DirectXTex)
                              → normalize                  (NormalMap)
                              → CompressToScratchImage     (Codec)  ← shared
                              → write container            (ContainerWrite)
```

## How an open flows

```
PS: ReadStart selector
  └─ IntelPlugin::PluginMain  →  PhotoshopRead::DoReadStart
        ├─ sniff "TEX" magic ?
        │     ├─ yes → parse TEX header → BC1/BC3 decode (s3tc) → RGBA
        │     └─ no  → LoadFromDDSMemory → Decompress/Convert (DirectXTex)
        └─ DoReadContinue streams each image / cube face / mip into PS buffer
```

---

## Why this split

- **The codec is written once.** DDS and TEX share the exact same encode
  (`CompressToScratchImage`); only the container/header differs. Keeping the
  codec in its own file makes that reuse explicit and prevents the two writers
  from drifting apart.
- **Host vs. core.** Anything that touches `ps.formatRecord`, suites, or
  selectors lives in the host layer. The format/codec core can be reasoned about
  (and eventually unit-tested) without a running Photoshop.
- **Small files = safe edits.** Adding a format (e.g. BC7 tuning) or fixing a
  cube-map bug means opening one focused file, not scrolling a 3,500-line monolith.

## Build

`build.ps1` (MSBuild, x64 Release) → `Plugins\x64\RitoTex.8bi`. Every module is a
plain `ClCompile` entry in `IntelTextureWorks.vcxproj`.
