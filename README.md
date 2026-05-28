<div align="center">

<img src="https://readme-typing-svg.demolab.com?font=Fira+Code&weight=700&size=40&duration=2800&pause=900&color=C8AA6E&center=true&vCenter=true&width=620&height=80&lines=RitoTex;Photoshop+%C3%97+League+of+Legends;.tex+textures%2C+natively" alt="RitoTex" />

### Open, edit, and save League of Legends `.tex` textures **directly in Adobe Photoshop**

<br/>

<img src="https://img.shields.io/badge/version-2.0.0-C8AA6E?style=for-the-badge" alt="version" />
<img src="https://img.shields.io/badge/platform-Windows%20x64-0A1428?style=for-the-badge&logo=windows&logoColor=white" alt="platform" />
<img src="https://img.shields.io/badge/Photoshop-CS6%E2%80%93CC%202025-31A8FF?style=for-the-badge&logo=adobephotoshop&logoColor=white" alt="photoshop" />
<img src="https://img.shields.io/badge/license-Apache%202.0-005A82?style=for-the-badge" alt="license" />

<br/><br/>

![divider](https://raw.githubusercontent.com/andreasbm/readme/master/assets/lines/rainbow.png)

</div>

## ✨ What is RitoTex?

RitoTex is a native Adobe Photoshop **File Format plug-in** (`.8bi`) that teaches Photoshop to speak Riot Games' `.tex` texture format. Once installed, `.tex` files behave like any other image:

```
File ▸ Open  ……  pick a .tex  ……  edit like a PSD
File ▸ Save As  ……  type  skin.tex  ……  done
```

No exporting. No converting. No round-tripping through `.dds`. It also reads and writes `.dds`, so it's a one-stop texture pipeline for League modding.

<div align="center">
<table>
<tr>
<td align="center">🎮<br/><b>Native .tex</b><br/><sub>Open / Save As</sub></td>
<td align="center">🧱<br/><b>BC1·3·5·7</b><br/><sub>+ BGRA8</sub></td>
<td align="center">🗺️<br/><b>Mipmaps</b><br/><sub>auto or from layers</sub></td>
<td align="center">🧬<br/><b>Normal maps</b><br/><sub>flip &amp; normalize</sub></td>
<td align="center">🔔<br/><b>Update check</b><br/><sub>GitHub Releases</sub></td>
</tr>
</table>
</div>

<div align="center">

![divider](https://raw.githubusercontent.com/andreasbm/readme/master/assets/lines/rainbow.png)

</div>

## 🚀 Features

| | Feature | Notes |
|:--:|---|---|
| 🎮 | **Native `.tex` open & save** | Registered as a real Photoshop file format — `File ▸ Open` / `Save As` |
| 🟦 | **`.dds` support** | Full DDS load/save via DirectXTex (BC1–BC7) |
| 🧱 | **BC1 / BC3 / BC5 / BC7** | DXT1, DXT5, two-channel (normal maps), and high-quality BC7. Optional Floyd–Steinberg dithering on BC1/BC3 |
| 🗺️ | **Mipmaps** | Auto-generate, or build the chain from Photoshop layers |
| 🧊 | **Cube maps** | Cross / layer layouts (DDS) |
| 🧬 | **Normal-map tools** | Channel flip (X/Y) + renormalization |
| 🔍 | **Compression preview** | Compare original vs compressed before saving |
| ⚡ | **Intel ISPC encoder** | Fast, multi-threaded texture compression |
| 🔔 | **Built-in update checker** | Quietly checks GitHub Releases, links you to the new version |

<div align="center">

![divider](https://raw.githubusercontent.com/andreasbm/readme/master/assets/lines/rainbow.png)

</div>

## 📦 Installation

> **Requires:** 64-bit Adobe Photoshop (CS6 or newer).

1. **Grab** `RitoTex.8bi` from the [**Releases**](https://github.com/RitoShark/RitoTex-Photoshop/releases) page (or build it yourself — see below).
2. **Drop it in** your Photoshop File Formats folder:
   ```
   C:\Program Files\Adobe\Adobe Photoshop <version>\Plug-ins\File Formats\
   ```
3. **Restart Photoshop.**

That's it. `.tex` now shows up in Open and Save As.

<div align="center">

![divider](https://raw.githubusercontent.com/andreasbm/readme/master/assets/lines/rainbow.png)

</div>

## 🎨 Usage

<details open>
<summary><b>Open a texture</b></summary>

`File ▸ Open` → pick any `.tex` or `.dds`. It decodes straight onto a layer — no prompts, no "load alpha as…?" dialog. BC1/BC3/BC5/BC7 and uncompressed are all auto-detected.
</details>

<details>
<summary><b>Save a texture</b></summary>

1. `File ▸ Save As`, choose **RitoTex**.
2. Type a filename ending in **`.tex`** or **`.dds`** — the format is auto-detected from the extension.
3. Pick compression (**BC1** for RGB, **BC3** for RGBA), mipmap mode, and dithering.
4. **OK.**

> ⚠️ Block-compressed formats require width & height to be **multiples of 4** (a League requirement).
</details>

<details>
<summary><b>Dithering</b></summary>

Enable **Error Diffusion Dithering** to reduce banding on BC1/BC3. Choose **Perceptual** (luma-weighted) or **Uniform** error metrics.
</details>

<div align="center">

![divider](https://raw.githubusercontent.com/andreasbm/readme/master/assets/lines/rainbow.png)

</div>

## 🧩 The `.tex` format

```text
┌──────────────────────────────── 12-byte header ────────────────────────────────┐
│ off  size  field         value                                                   │
├──────────────────────────────────────────────────────────────────────────────── │
│ 0x00   4   magic         "TEX\0"   (54 45 58 00)                                  │
│ 0x04   2   width         uint16  (little-endian)                                 │
│ 0x06   2   height        uint16  (little-endian)                                 │
│ 0x08   1   unk1          (reserved)                                              │
│ 0x09   1   format        0x0A=BC1 · 0x0C=BC3 · 0x0D=BC7 · 0x0E=BC5 · 0x14=BGRA8 │
│ 0x0A   1   unk2          (reserved)                                              │
│ 0x0B   1   has_mipmaps   0 = none · 1 = present                                  │
└──────────────────────────────────────────────────────────────────────────────── ┘

payload:  mipmaps stored SMALLEST → LARGEST, full-resolution image last
```

> 📁 Full format notes, the SDK map, and the reference-backend audits live in [`docs/`](docs/).

<div align="center">

![divider](https://raw.githubusercontent.com/andreasbm/readme/master/assets/lines/rainbow.png)

</div>

## 🔔 Update checking

RitoTex checks [GitHub Releases](https://github.com/RitoShark/RitoTex-Photoshop/releases) for a newer build — quietly and respectfully:

- 🧵 Runs on a **background thread** — never slows down opening or saving.
- ⏱️ **Throttled to once / 24h** per machine (`HKCU\Software\RitoTex`).
- 🔕 **Fails silent** when you're offline or rate-limited.
- 🔗 **Link-only** — it never downloads or runs anything; it just points you to the release page.
- 🤫 Won't nag: dismiss or "skip this version" and it remembers.

The check is kicked off on the first **Open** or **Save As** of a session (from `ReadStart` / `WriteStart`).

> 🐞 **Debugging the update check.** The checker traces every step to the
> Windows debug channel. Run [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview)
> (Sysinternals) as admin, enable *Capture ▸ Capture Global Win32*, and filter on
> `RitoTex/Update`. You'll see lines like `spawning background update worker`,
> `latest tag = v2.1.0, installed = 2.0.0`, and `UPDATE AVAILABLE`.
>
> **Release builds throttle to once/24h**, so re-testing needs a reset:
> ```powershell
> reg delete "HKCU\Software\RitoTex" /v LastCheckUnix /f
> ```
> **Debug builds bypass the throttle entirely** — every launch hits GitHub. (Or
> define `RITOTEX_FORCE_UPDATE_CHECK` to force it in a Release build.)
>
> Build the debug plugin with **`.\dev.ps1`** (Debug|x64, defines `_DEBUG`). It
> copies the same `Plugins\x64\RitoTex.8bi` that `build.ps1` produces — so
> whichever script you ran last is the installed one. Re-run `build.ps1` to get
> the throttled Release build back.

Design details: [`docs/UpdateChecking_Design.md`](docs/UpdateChecking_Design.md).

<div align="center">

![divider](https://raw.githubusercontent.com/andreasbm/readme/master/assets/lines/rainbow.png)

</div>

## 🛠️ Building from source

<b>Requirements</b>

- Visual Studio 2022 (**v143** toolset) + Windows 10 SDK
- Adobe Photoshop SDK — expected at `..\PhotoshopSDK` (the build script also accepts an in-folder copy or a preset `PHOTOSHOP_SDK_CS6`)
- Intel ISPC compiler (bundled: `3rdParty/Intel/Tools/ispc.exe`)

<b>Build</b>

```powershell
.\build.ps1
```

Output: `Plugins\x64\RitoTex.8bi`

<details>
<summary><b>Project layout</b></summary>

The plug-in is split into small, single-responsibility modules. `IntelPlugin.cpp`
is now just the class core + the Photoshop selector dispatch; everything else
lives in its own file. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the
full module map and how data flows through them.

```
RitoTex-Photoshop/
├─ IntelCompressionPlugin/        # the plug-in
│  │  ── host layer (Photoshop-facing) ──
│  ├─ IntelPlugin.cpp/.h          # class core + PluginMain selector dispatch
│  ├─ PhotoshopRead.cpp           # .tex/.dds load + parse (DoRead*, composite layers)
│  ├─ PhotoshopWrite.cpp          # save orchestration + .tex/.dds format routing
│  ├─ Preview.cpp                 # save-dialog preview render + size stats
│  ├─ Scripting.cpp               # Actions / descriptor read-write (batch)
│  │  ── format / codec core (host-agnostic) ──
│  ├─ TexFormat.cpp/.h            # .tex header, format codes, DXGI↔tex maps
│  ├─ Codec.cpp                   # shared BC compress/decompress dispatch
│  ├─ ContainerWrite.cpp          # DDS + TEX file writers
│  ├─ CubeMap.cpp                 # cube ↔ cross / layer assembly
│  ├─ NormalMap.cpp               # channel flip + renormalize
│  ├─ LayerOps.cpp                # layer channel reads, mips-from-layers
│  ├─ PixelConvert.cpp            # Photoshop buffer (8/16/32-bit) → RGBA8
│  ├─ s3tc.cpp                    # DXT1/DXT5 CPU decode (.tex read path)
│  │  ── support ──
│  ├─ UpdateChecker.cpp/.h        # GitHub-Releases update check (WinHTTP)
│  ├─ IntelPlugin.r / .pipl       # PiPL — registers .tex & .dds formats
│  ├─ IntelPluginName.h           # name + version (single source of truth)
│  ├─ kernel.ispc                 # Intel ISPC BC encoder
│  └─ *Dialog.cpp                 # save / preview / options UI
├─ 3rdParty/                      # DirectXTex + Intel ISPC
├─ docs/                          # ARCHITECTURE, format notes, SDK map, audits
└─ build.ps1                      # one-command build
```
</details>

<div align="center">

![divider](https://raw.githubusercontent.com/andreasbm/readme/master/assets/lines/rainbow.png)

</div>

## 📄 License

**Apache License 2.0** — see [`license.txt`](license.txt).

Built on the [Intel® Texture Works Plugin](https://github.com/GameTechDev/Intel-Texture-Works-Plugin) (© 2017 Intel Corporation, Apache 2.0) with bundled [DirectXTex](https://github.com/microsoft/DirectXTex) (MIT).

<div align="center">

<br/>

**Made for the League of Legends modding community by [RitoShark](https://github.com/RitoShark)** 🦈

<sub>Riot Games and League of Legends are trademarks of Riot Games, Inc. RitoTex is an unofficial, fan-made tool and is not affiliated with or endorsed by Riot Games.</sub>

</div>
