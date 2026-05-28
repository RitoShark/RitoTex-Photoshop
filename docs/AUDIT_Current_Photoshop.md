# AUDIT — Current In-Progress Photoshop Plugin (`RitoTex-Photoshop`)

**Audited folder:** `e:\RitoShark\RitoTex\RitoTex-Photoshop`
**Audit date:** 2026-05-28
**Purpose:** Inventory exactly what exists, what works, and what's broken/incomplete, ahead of a clean rebuild of the Photoshop plugin.

> ## TL;DR (read this first)
> This folder is a **fork of Intel® Texture Works** (Adobe Photoshop File-Format `.8bi` plugin), rebranded "RitoTex v2.0.0" and **substantially extended with full League `.tex` read AND write support**. The codebase is **largely complete and coherent** — `IntelPlugin.cpp` is ~3,430 lines with real implementations of the entire Format-plugin selector lifecycle, DDS load/save (DirectXTex), TEX load/save (custom), the ISPC + DirectXTex compression pipeline, cube maps, mipmaps, normal-map processing, Floyd-Steinberg dithering, a Win32 save dialog, and a compression preview window. The PiPL dual-registers `.dds` and `.tex` formats. The build is configured for VS2022 / v143 / x64 with proper SDK include paths, an ispc custom build, and a Cnvtpipl PiPL step.
>
> **No structural blocker in the source itself.** (The TEX load state `struct TexLoadInfo { ... } texLoadInfo;` and the helper `get_num_mipmaps()` are defined at **file scope in `IntelPlugin.cpp:2435-2442`**, before the functions that use them — they are a TU-local global + free function, not class members, so the committed headers are consistent with the source. The earlier worry about a header/source desync does **not** hold.) The remaining obstacles to a clean build are **configuration**, not code: `build.ps1` points `PHOTOSHOP_SDK_CS6` at a non-existent in-folder `PhotoshopSDK` (the SDK is one level up at `..\PhotoshopSDK`), and the vcxproj's **x64-Debug** config hard-codes a stale absolute SDK path. Fix those two and it should compile (modulo any normal first-build warnings against this SDK/toolset).
>
> **Bottom line:** this is a real, essentially-complete plugin — not a skeleton. For the rebuild it is a strong reuse/extend candidate, not a throwaway. The main liabilities are TEX-format correctness details (unpacked header struct, unresearched `unk1`/`unk2`, no BC7/ETC, 3-byte magic check, mips dropped on load), the two build-path nits, and inherited Intel-code rough edges. Update checking is absent.

---

## 1. What kind of plugin is this?

**An Adobe Photoshop C++ Plug-in SDK *File Format* module** (output `.8bi`, a Windows DLL), forked from the **Intel® Texture Works Plugin** (© 2017 Intel, Apache-2.0; discontinued).

Evidence:
- **SDK headers** (`IntelPlugin.h:21-24`): `<PIExport.h>`, `<PIUtilities.h>`, `<PIColorSpaceSuite.h>`, `<FileUtilities.h>`. The SDK is present at `e:\RitoShark\RitoTex\PhotoshopSDK\pluginsdk\photoshopapi\photoshop\` (confirmed `PIExport.h`, `PIFormat.h`).
- **Type = Format** (not Filter/Automation): PiPL `Kind { ImageFormat }` (`IntelPlugin.r:97,176`); dispatcher switches on `formatSelector*` (`IntelPlugin.cpp:3201-3276`); `aete` inherits `classExport` (`IntelPlugin.r:271`).
- **Output `.8bi`**: vcxproj `<ConfigurationType>DynamicLibrary` + `<TargetExt>.8bi` (lines 29/84-87); `<ProjectName>RitoTex` (line 24).
- **Dependencies bundled:** DirectXTex (`3rdParty/DirectXTex/`) for DDS/BCn; Intel ISPC (`3rdParty/Intel/`, `kernel.ispc`, `ispc.exe`) for fast BC1/BC3/BC6H/BC7/ETC1 encoding.
- Windows-only in practice (PiPL has Mac arms, but `build.ps1`/vcxproj are MSBuild x64; UI uses Win32 `HWND`/`WriteFile`/`GetFileSizeEx`).

---

## 2. Entry point & PiPL

### Entry point
- Exported symbol **`PluginMain`** (`IntelPlugin.cpp:3304`, `DLLExport MACPASCAL`), wrapped in `try/catch(...)` → `*result = -1` on exception, forwarding to `IntelPlugin::GetInstance().PluginMain(...)` (`:3311`).
- Class method `IntelPlugin::PluginMain` at `:3133`. Singleton via `GetInstance()` (`:29`). State held in `ps` (formatRecord/result/data) + `preview` + `loadInfo` (+ the missing `texLoadInfo`).
- PiPL names the symbol on Win64 via `CodeWin64X86 { "PluginMain" }` (`IntelPlugin.r:111,190`).

### PiPL (`IntelCompressionPlugin/IntelPlugin.r`) — dual registration

| Item | DDS PiPL (`ResourceID_DDS = 16000`) | TEX PiPL (`ResourceID_TEX = 16001`) |
|---|---|---|
| Kind | `ImageFormat` | `ImageFormat` |
| Name | `"RitoTex DDS"` | `"RitoTex TEX"` |
| Code | `PluginMain` (Win64) | `PluginMain` (Win64) |
| FmtFileType | `'DDS ', 'DDSX'` | `'TEX ', 'DDSX'` |
| Read/WriteExtensions | `'DDS '` | `'TEX '` |
| FilteredTypes / FilteredExtensions | `'DDSX'` / `'DDS '` | `'TEX '` / `'TEX '` |
| FormatFlags | NotSaveImageResources, CanRead, CanWrite, CanWriteIfRead, CanWriteTransparency, CanCreateThumbnail | same |
| SupportedModes | Gray, RGB, Multichannel | same |
| EnableInfo | `in(...Gray,RGB,Multichannel) \|\| depth==16 \|\| depth==32` | same |
| PlugInMaxSize / FormatMaxSize | 32767×32767 | same |
| FormatLayerSupport | `doesSupportFormatLayers` (mips/cube faces as layers) | same |

Other resources: `'aete'` dictionary (`:254`) — suite `'sdK5'`, class `'texX'`, props `preset`('pres')/`mipmap`('mipm')/`alphaseprate`('alps'); `kPrompt`="RitoTex export to file:"; creator "RitoShark RitoTex Plugin". Name "RitoTex", version " v2.0.0" (`IntelPluginName.h:24-25`).

**Discovery:** Photoshop reads the embedded PiPLs and registers two Format handlers — `RitoTex DDS` (claims `.dds`) and `RitoTex TEX` (claims `.tex`) — for Open/Save As.

> The human-readable `.r` is compiled to binary `IntelPlugin.pipl` by the vcxproj CustomBuild (cl `/EP` → `.rr` → `Cnvtpipl.exe` → `.pipl`), which `IntelPlugin.rc` `#include`s (`.rc:281`). `*.pipl` is git-ignored and absent (regenerated at build). This step is correctly wired in the vcxproj.

---

## 3. Format selector dispatch — implemented vs stubbed

Dispatch in `IntelPlugin::PluginMain` switch (`IntelPlugin.cpp:3201-3276`). **All handlers are real**; the only intentionally-empty ones are Continue/Finish phases that have nothing to do (normal for this design).

| Selector | Handler / line | Status |
|---|---|---|
| `formatSelectorAbout` | `ShowAboutIntel` (`:3155-3160`) | ✅ Real |
| `ReadPrepare` | `DoReadPrepare` (`:2452`) | ✅ Real (inits `loadInfo`+`texLoadInfo`, maxData=0) |
| `ReadStart` | `DoReadStart` (`:2471`) | ✅ Real — sniffs `"TEX"` magic; TEX branch vs DDS branch (`LoadFromDDSMemory`) |
| `ReadContinue` | `DoReadContinue` (`:2789`) | ✅ Real — TEX: s3tc decode + RGBA→ARGB + `advanceState`; DDS: per-image/mip/cube copy with 8/16/32-bit paths |
| `ReadFinish` | `DoReadFinish` (`:3007`) | ✅ Real — frees `loadInfo.readImagePtr` (TEX path freed in Continue) |
| `OptionsPrepare/Start/Continue/Finish` | inline (`:3216-3225`) | ✅ Present (maxData=0 / data=NULL; minimal by design — options gathered in WriteStart dialog) |
| `EstimatePrepare/Start/Continue/Finish` | inline (`:3228-3242`) | ✅ Real — Start sets `minDataBytes=area/2`, `maxDataBytes=area*4` |
| `WritePrepare` | `DoWritePrepare` (`:1622`) | ✅ Real (maxData=0, layerData=0 → plugin reads layers itself) |
| `WriteStart` | `DoWriteStart` (`:1684`) | ✅ Real — shows `CustomSaveDialog`, then routes TEX vs DDS via `IsSavingToTEXFormat` (finalSpec extension) |
| `WriteContinue` | `DoWriteContinue` (`:1725`) | empty by design (data already written in Start) |
| `WriteFinish` | `DoWriteFinish` (`:1730`) | ✅ Real — `WriteScriptParamsForWrite()` |
| `FilterFile` | `DoFilterFile` (`:2422`) | ✅ Present (format-sniff for Open) |
| `ReadLayerStart` | `DoReadLayerStart` (`:3017`) | ✅ Real (mip/cube-as-layer naming) |
| `ReadLayerContinue` | → `DoReadContinue` | ✅ shared path |
| `ReadLayerFinish`, `WriteLayer*` | empty | by design (no layered write) |

> Important correction to any prior assumption: **`DoOptionsStart` and `DoEstimateStart` are implemented inline in the switch** (`:3219`, `:3231`), not missing. The plugin does its option-gathering in the `WriteStart` dialog rather than the Options selectors, which is a valid pattern Intel uses.

---

## 4. The `.tex` format (as implemented)

### 4.1 Definitions (`IntelCompressionPlugin/IntelPlugin.h:130-150`)
```cpp
enum tex_format : uint8_t {
    tex_format_etc1=0x1, tex_format_etc2_eac=0x2, tex_format_etc2=0x3,
    tex_format_dxt1=0xA /*BC1*/, tex_format_dxt5=0xC /*BC3*/, tex_format_bgra8=0x14
};
#define tex_magic "TEX"
typedef struct {
    uint8_t  magic[4];   uint16_t image_width; uint16_t image_height;
    uint8_t  unk1;       tex_format tex_format; uint8_t unk2; bool has_mipmaps;
} TEX_HEADER;          // 12 bytes
```

### 4.2 Byte-offset table (12-byte header)
| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0x00 | 4 | magic | char[4] | `54 45 58 00` = `"TEX\0"` (reader only checks first **3** bytes: `memcmp(...,"TEX",3)`, `:2502`) |
| 0x04 | 2 | image_width | uint16 LE | |
| 0x06 | 2 | image_height | uint16 LE | |
| 0x08 | 1 | unk1 | uint8 | written 0; meaning unresearched |
| 0x09 | 1 | tex_format | uint8 | 0x0A DXT1 / 0x0C DXT5 / 0x14 BGRA8 / 0x01-0x03 ETC* (defined, **not handled**) |
| 0x0A | 1 | unk2 | uint8 | written 0; meaning unresearched |
| 0x0B | 1 | has_mipmaps | uint8(bool) | 0 none / 1 present |

### 4.3 Data layout & code paths
- Data follows the header. **Mips stored smallest→largest; full image LAST** (mip 0 after all smaller mips).
  - **Write** (`WriteTEXFile` `:2064`, `WriteTEXMipmaps` `:2027`): header → `for mip=mipCount-1..1` (smallest first) → mip 0 last. Uses raw `WriteFile` on `dataFork`.
  - **Read** (`DoReadStart` `:2502-2604`): parses header, computes mip-0 block size (`blockSize = dxt5?16:8`, blockW/H = `(dim+3)/4`), and when `has_mipmaps` **seeks past all smaller mips** (`get_num_mipmaps()` + skip loop `:2526-2548`) to read only the full-res mip 0. So Photoshop loads only the main image (mips are skipped on load, not exposed as layers for TEX).
- **Decode** (`DoReadContinue` `:2841-2846`): CPU `BlockDecompressImageDXT5`/`DXT1` from `s3tc.cpp`, then **RGBA→ARGB reorder** (`:2857-2870`) before handing to Photoshop via `advanceState`. BGRA8 path: "no need to decompress" (passes through).
- **Encode** (`DoWriteTEX` `:2110`): reuses the shared pipeline — `CopyDataForEncoding` → optional `GenerateMipMaps` → `CompressToScratchImage` (ISPC for BC1/BC3, DirectXTex `Compress` when dithering on) → `BuildTEXHeader` → `WriteTEXFile`. Cube maps explicitly rejected for TEX ("use DDS instead", `:2133`). Format chosen by `GetTEXFormatFromDXGI` (`:1991`: BC1→dxt1, BC3→dxt5, RGBA/BGRA→bgra8, else→dxt5).
- Format routing on save: `IsSavingToTEXFormat` (`:1634`) reads `formatRecord->finalSpec` (UTF-16 final path; PS writes to `.tmp` first so it must use finalSpec, not dataFork), checks a 3-char `.tex` extension case-insensitively.

### 4.4 Cross-reference with format docs
**No `TEXTURE_FORMAT.md` exists in this folder.** Format docs are: `README.md:54-67` (matches the table) and `TEX_SAVE_IMPLEMENTATION.md` (design/summary; describes same header + reverse-mip order). Both agree with the code.

### 4.5 Correctness caveats (matter for the rebuild)
- **`TEX_HEADER` is NOT `#pragma pack(1)`.** `sizeof(TEX_HEADER)` (12) is used directly as the on-disk read/write size (`:2067`, `:2490`). It happens to be 12 on MSVC, but `bool has_mipmaps` (1 byte, not guaranteed) and the `uint16` after `char[4]` make this fragile — the rebuild should pack the struct or serialize fields explicitly.
- **`unk1`/`unk2` unresearched** (always 0). Real League `.tex` files may carry meaningful values; round-tripping could corrupt or mislabel.
- **ETC1/ETC2 (0x01-0x03) defined but unhandled** on both read (falls through, no decode) and write (everything unknown → DXT5). The ISPC kernel *can* encode ETC1, but it's not wired into the TEX path.
- **No BC7/BC6H in the TEX path** (enum has no BC7; write maps to DXT5). Note BC7 *encoding* exists in `kernel.ispc` and DDS may use it.
- Magic check is only 3 bytes, so a file starting `"TEXxyz..."` without the NUL would be misread as TEX.

---

## 5. Build system

- **Solution** `IntelTextureWorks.sln`: two projects — `DirectXTex` (`3rdParty/.../DirectXTex_Desktop_2012.vcxproj`) and `IntelTextureWorks`→`RitoTex` (`IntelCompressionPlugin/IntelTextureWorks.vcxproj`). Configs Debug/Profile/Release × Win32/x64.
- **`IntelTextureWorks.vcxproj` is VALID** (UTF-8 BOM, well-formed MSBuild XML, 415 lines). Toolset **v143**, `WindowsTargetPlatformVersion 10.0`, `DynamicLibrary`, `TargetExt .8bi`, `ProjectName RitoTex`.
  - **Source list** (`:309-349`): all SDK common sources (`PIUtilities*.cpp`, `FileUtilities*.cpp`, `PIDLLInstance.cpp`, `PIUSuites.cpp`, `PIWinUI.cpp`, `DialogUtilitiesWin.cpp`), `ispc_texcomp.cpp`, `win32Threads.cpp`, `s3tc.cpp`, `SaveOptionsDialog.cpp`, `CustomSaveDialog.cpp`, `IntelPlugin.cpp`, `IntelPluginUIWin.cpp`, `PreviewDialog.cpp` + headers.
  - **Include dirs** (`:110` etc.): `.;..\3rdParty\Intel\Source;..\3rdParty\DirectXTex\DirectXTex;$(PHOTOSHOP_SDK_CS6)\pluginsdk\PhotoshopAPI\Photoshop;...\PICA_SP;...\samplecode\common\includes`.
  - **Custom builds:** `IntelPlugin.r` → cl `/EP` `.rr` → `Cnvtpipl.exe` → `.pipl` (`:352-381`); `kernel.ispc` → `ispc -O2 ... --target=sse2,sse4,avx,avx2` (x64/x86) (`:392-402`).
  - Links `odbc32/odbccp32/version.lib`. Project-references the DirectXTex vcxproj.
  - ⚠️ The **x64 Debug** config block hard-codes a stale absolute SDK path `C:\Users\GuiSai\Desktop\adobe_photoshop_cs6_sdk_win\...` (`:208`) instead of `$(PHOTOSHOP_SDK_CS6)` — leftover from the original author. Other configs use the env var correctly.
- **`build.ps1`**: sets `PHOTOSHOP_SDK_CS6 = $PSScriptRoot\PhotoshopSDK` — **wrong location** (no `PhotoshopSDK` inside `RitoTex-Photoshop`; the SDK is one level up at `..\PhotoshopSDK`). Then MSBuild `Release|x64`, expects `Plugins\x64\RitoTex.8bi`.

### Does it build? **Not out-of-box — two CONFIG fixes needed (no source defect):**
1. **SDK path (build.ps1).** `build.ps1:15` sets `PHOTOSHOP_SDK_CS6 = $PSScriptRoot\PhotoshopSDK`, but there is no `PhotoshopSDK` inside `RitoTex-Photoshop`; the SDK is one level up at `e:\RitoShark\RitoTex\PhotoshopSDK`. Point it there (or vendor the SDK in).
2. **Stale absolute path (vcxproj).** The x64-**Debug** ItemDefinitionGroup hard-codes `C:\Users\GuiSai\Desktop\adobe_photoshop_cs6_sdk_win\...` (`vcxproj:208`) instead of `$(PHOTOSHOP_SDK_CS6)`. The other three configs (Debug/Release Win32, Release x64) correctly use the env var. Replace it.

**Source consistency note:** `IntelPlugin.cpp` uses `texLoadInfo` and `get_num_mipmaps()`, which are **defined at file scope in the same .cpp at `:2435-2442`** (a TU-local `struct TexLoadInfo {...} texLoadInfo;` global + a free function), *not* class members — so the committed `IntelPlugin.h` is consistent and there is **no missing-declaration compile error**. Everything else the .cpp calls (`errorMessage`, `ShowLoadDialog`, `ShowAboutIntel`) is declared in `IntelPluginUIWin.h`.

A stale `IntelTextureWorks.VC.db` (1.9 MB IntelliSense cache) is committed. No built artifacts (`Plugins/`, `*.8bi` git-ignored & absent).

---

## 6. State assessment — works / stubbed / broken / missing

| Area | Status | Location |
|---|---|---|
| PS Format module design | ✅ correct | `IntelPlugin.h:21`, PiPL |
| PiPL dual DDS+TEX registration | ✅ Real | `IntelPlugin.r` |
| Entry point + full selector lifecycle | ✅ Real | `IntelPlugin.cpp:3133-3301` |
| About box | ✅ Real | `ShowAboutIntel`, `IDD_ABOUT` |
| `.tex` header parse (read) | ✅ Real | `DoReadStart:2502-2604` |
| `.tex` → Photoshop (decode + ARGB + deliver) | ✅ Real | `DoReadContinue:2817-2894` (uses `s3tc.cpp`) |
| `.tex` mip skip on load | ✅ Real (mips skipped, only mip0 loaded) | `:2526-2548` |
| `.tex` header build + format map | ✅ Real | `BuildTEXHeader:2012`, `GetTEXFormatFromDXGI:1991` |
| `.tex` write (header + reverse mips + mip0) | ✅ Real | `WriteTEXFile:2064`, `WriteTEXMipmaps:2027`, `DoWriteTEX:2110` |
| Save-format routing (.tex/.dds by finalSpec) | ✅ Real | `IsSavingToTEXFormat:1634`, `DoWriteStart:1684` |
| DDS read (DirectXTex, mips/cube/alpha/8-16-32bit) | ✅ Real | `DoReadStart:2606-2786`, `DoReadContinue:2895-3004` |
| DDS write (DirectXTex SaveToDDSMemory) | ✅ Real | `DoWriteDDS:1848` |
| Compression pipeline (ISPC + DirectXTex dither) | ✅ Real | `CompressToScratchImage:127`, `ISPC_compression:629` |
| PS↔buffer conversion (8/16/32-bit) | ✅ Real | `ConvertToBCFrom8/16/32Bit:554-623`, `CopyDataForEncoding:88` |
| 4-pixel padding for BC blocks | ✅ Real | `DoPaddingToMultiplesOf4:691` |
| Cube maps (cross/layers, DDS only) | ✅ Real | `ConvertToCubeMap*:980-1253` |
| Mipmaps (autogen / from layers) | ✅ Real | `DoWriteDDS:1909`, `CopyLayersIntoMipMaps:1476` |
| Normal maps (flip X/Y, normalize) | ✅ Real | `FlipXYChannelNormalMap:1259`, `NormalizeNormalMapChain:1303` |
| Floyd-Steinberg dithering (BC1/BC3) | ✅ Real | `CompressToScratchImage:137-169` (`TEX_COMPRESS_DITHER`/`UNIFORM`) |
| Preview window (compare original vs compressed) | ✅ Real | `GetCompressedImageForPreview:731`, `PreviewDialog.cpp` (990 ln) |
| Save options dialog (BC3/BGRA, dither, mips) | ✅ Real | `CustomSaveDialog.cpp` (2000 ln), `SaveOptionsDialog.cpp` (1392 ln), `IDD_MAINDIALOG` |
| Scripting/batch (aete read/write params) | ✅ Real | `ReadScriptParamsFor*`, `WriteScriptParamsFor*` |
| s3tc DXT1/DXT5 CPU decoder | ✅ Real & correct | `s3tc.cpp` (251 ln) |
| ISPC encoder kernel (BC1/3/6H/7/ETC1) | ✅ Real (inherited) | `kernel.ispc` (3691 ln) |
| Estimate file-size selector | ✅ Real | `:3231-3237` |
| Header/source consistency | ✅ OK | `texLoadInfo`+`get_num_mipmaps` defined file-scope in `.cpp:2435-2442` |
| vcxproj x64-Debug SDK path | ⚠️ stale absolute path (config nit, not source) | `vcxproj:208` |
| build.ps1 SDK path | ⚠️ wrong (points inside folder) | `build.ps1:15` |
| `unk1`/`unk2` semantics | ❌ unresearched (hard 0) | header |
| TEX struct packing | ⚠️ not `pack(1)` | `IntelPlugin.h:142` |
| ETC1/ETC2 in TEX path | ❌ defined, unhandled | enum vs read/write |
| BC7/BC6H in TEX path | ❌ not mapped | `GetTEXFormatFromDXGI` |
| TEX mips as Photoshop layers (load) | ❌ skipped (mip0 only) | by design here |
| Update checking | ❌ missing entirely | none in tree |

### Brutally honest summary
- **This is a working-class implementation, not a stub.** The DDS path is essentially the complete Intel Texture Works plugin; the TEX path is a genuine, end-to-end addition (read decode + write encode + dual PiPL + format routing + UI branding). The source is internally self-consistent.
- **No source-level compile blocker** — the only obstacles to a clean build are two **configuration** issues: `build.ps1`'s wrong `PHOTOSHOP_SDK_CS6` path and the vcxproj x64-Debug stale absolute SDK path. Fix both and it should compile against the SDK present at `..\PhotoshopSDK`.
- **TEX-format fidelity is the real long-term risk:** unpacked header struct, unresearched `unk1`/`unk2`, no ETC/BC7, only-3-byte magic check, and mips dropped on load. These are correctness/feature gaps, not crashes.
- **Update checking is absent entirely** — nothing in the tree does version/network checks.
- The docs ("TEX_SAVE_IMPLEMENTATION.md: ✅ COMPLETE") are **directionally accurate** — the functions they describe genuinely exist and are wired in. Caveat the build-config fixes and the format-fidelity gaps above; the runtime behavior is otherwise plausibly functional once built.

---

## 7. Reuse vs rewrite (for the Reborn build)

### Strong reuse (this folder is a viable base)
- **`IntelPlugin.cpp`** — the whole selector lifecycle, DDS + TEX read/write, compression pipeline, cube/mip/normal handling. Keep; just add the missing `texLoadInfo`/`get_num_mipmaps` declarations.
- **`IntelPlugin.r` (PiPL + aete)** — correct dual DDS/TEX `ImageFormat` registration; the hardest part to get right. Reuse.
- **`.tex` model + read/write** — `TEX_HEADER`, enums, reverse-mip ordering, `s3tc.cpp` decoder, `WriteTEXFile`/`DoReadStart` logic. Reuse, but **pack the struct**, **research `unk1`/`unk2`** (cross-check the sibling GIMP/Paint.NET plugins and real `.tex` files), widen the magic check, and decide whether to expose load-time mips.
- **`kernel.ispc` + `3rdParty/Intel` + `3rdParty/DirectXTex`** — encoder stack. Reuse wholesale; the ispc custom-build step already exists in the vcxproj.
- **Win32 UI** — `CustomSaveDialog.cpp` (2000 ln), `SaveOptionsDialog.cpp` (1392), `PreviewDialog.cpp` (990), `IntelPluginUIWin.cpp` (335), dialogs in `IntelPlugin.rc` (RitoTex-themed). Reuse.
- The build project itself is reusable after the two path fixes.

### Fix / regenerate (don't throw away)
- **`build.ps1:15`** → `..\PhotoshopSDK` (or vendor the SDK); **vcxproj x64-Debug** include path → `$(PHOTOSHOP_SDK_CS6)`.
- (Optional cleanup) `texLoadInfo` is a TU-local global in `IntelPlugin.cpp` (`:2435`); for the rebuild, move it into the `IntelPlugin` class alongside `loadInfo` for symmetry/thread-safety hygiene. Not required to build.
- **`IntelTextureWorks.VC.db`** — delete and git-ignore.
- **TEX correctness hardening** (pack struct, unk fields, ETC/BC7, magic check) — incremental, not a rewrite.

### Throw away
- Nothing structural. Optionally rename Intel-era identifiers (`IntelPlugin*`, `IntelTextureWorks`) to RitoTex for clarity, but that's cosmetic.

### Net recommendation
**Extend this folder, don't restart.** It is an essentially-complete, correctly-architected Photoshop Format plugin with real TEX support and no source-level compile blocker. The fastest path to a shipping RitoTex Photoshop plugin is: (1) fix the two SDK build paths so it compiles, (2) harden TEX-format fidelity (`unk1`/`unk2`, struct packing, BC7/ETC, widen magic check, decide on load-time mips), (3) add update checking (currently absent), and (4) optionally tidy the TU-local `texLoadInfo` into the class. A from-scratch re-fork of upstream Intel Texture Works would discard the substantial, working TEX integration already present here.

---

## 8. File inventory (non-3rdParty, non-sample)

| Path | Role | Reality |
|---|---|---|
| `IntelCompressionPlugin/IntelPlugin.cpp` | entry point, selectors, TEX/DDS read+write, pipeline | ✅ Real, ~3,430 ln (near-complete) |
| `IntelCompressionPlugin/IntelPlugin.h` | class, Globals, `TEX_HEADER`, enums | ✅ Real & consistent (`texLoadInfo`/`get_num_mipmaps` live file-scope in the `.cpp`, by design) |
| `IntelCompressionPlugin/IntelPluginName.h` | name/version/scripting keys | ✅ Real |
| `IntelCompressionPlugin/IntelPlugin.r` | PiPL (DDS+TEX) + aete + strings | ✅ Real (`.pipl` built at compile) |
| `IntelCompressionPlugin/IntelPlugin.rc` | Win32 dialogs, version, icons, `#include .pipl` | ✅ Real |
| `IntelCompressionPlugin/resource.h` | control IDs | ✅ Real |
| `IntelCompressionPlugin/s3tc.{cpp,h}` | DXT1/DXT5 CPU decoder | ✅ Real, used by TEX load |
| `IntelCompressionPlugin/kernel.ispc` | Intel ISPC BC1/3/6H/7/ETC1 encoder | ✅ Real (3691 ln) |
| `IntelCompressionPlugin/CustomSaveDialog.{cpp,h}` | main save dialog | ✅ Real (2000 ln) |
| `IntelCompressionPlugin/SaveOptionsDialog.{cpp,h}` | save options | ✅ Real (1392 ln) |
| `IntelCompressionPlugin/PreviewDialog.{cpp,h}` | compression preview | ✅ Real (990 ln) |
| `IntelCompressionPlugin/IntelPluginUIWin.{cpp,h}` | Win UI helpers (`errorMessage`/`ShowLoadDialog`/`ShowAboutIntel`) | ✅ Real (335 ln) |
| `IntelCompressionPlugin/IntelTextureWorks.vcxproj` | VS project | ✅ Valid (one stale x64-Debug path) |
| `IntelCompressionPlugin/IntelTextureWorks.vcxproj.filters` | IDE grouping | ✅ Real |
| `IntelTextureWorks.sln` | solution | ✅ Real |
| `IntelTextureWorks.VC.db` | IntelliSense cache | stale artifact (ignore) |
| `build.ps1` | build driver | ⚠️ wrong SDK path |
| `README.md` / `IMPLEMENTATION_PLAN.md` / `TEX_SAVE_IMPLEMENTATION.md` | docs | ✅ accurate-ish (status slightly oversold) |
| `3rdParty/DirectXTex/**`, `3rdParty/Intel/**` | libraries | ✅ upstream |
| `PhotoshopScripts/*.jsx` | cube-map helper scripts | ✅ Real |
| `Sample Images/**` | test assets | ✅ Real |
| `PhotoshopSDK/` (expected by build) | **absent here** | SDK is at `..\PhotoshopSDK` (present) |

---

## 9. Key file:line index
- Entry: `IntelPlugin.cpp:3304` (`PluginMain`), `:3133` (method), `:3201-3276` (switch)
- TEX read: `:2471` (`DoReadStart`, magic at `:2502`), `:2789` (`DoReadContinue`, decode `:2841`)
- DDS read: `:2606-2786` (`LoadFromDDSMemory` at `:2644`)
- TEX write: `:1991` (`GetTEXFormatFromDXGI`), `:2012` (`BuildTEXHeader`), `:2027`/`:2064` (writers), `:2110` (`DoWriteTEX`)
- DDS write: `:1848` (`DoWriteDDS`, `SaveToDDSMemory` `:1964`)
- Save routing: `:1634` (`IsSavingToTEXFormat`), `:1684` (`DoWriteStart`)
- Pipeline: `:127` (`CompressToScratchImage`), `:629` (`ISPC_compression`)
- Format model: `IntelPlugin.h:130-150`
- TEX load state (file-scope, defined in .cpp): `struct TexLoadInfo {...} texLoadInfo;` `IntelPlugin.cpp:2435-2440`; `get_num_mipmaps` `:2442`
- PiPL: `IntelPlugin.r:94-167` (DDS), `:173-246` (TEX), `:254-301` (aete)
- Build: `build.ps1:15` (bad SDK path), `vcxproj:208` (stale x64-Debug path), `:352-402` (PiPL+ispc custom builds)
