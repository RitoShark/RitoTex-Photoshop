# RitoTex Backend Audit — Paint.NET `.tex` FileType Plugin

**Audit target:** `e:\RitoShark\RitoTex\Paint.NET-Tex-Plugin`
**Purpose:** Reference-grade, reimplementation-quality dissection of the Paint.NET `.tex` backend so the new native C++ Photoshop plugin (Adobe File Format SDK) can reuse identical decode/encode logic.
**Date:** 2026-05-28
**Auditor note:** Every file in the target folder was read in full (README.md, LICENSE.md, TexFileTypePlugin.csproj, build.ps1 [absent], .gitignore, src\TexFile.cs, src\TexFileType.cs, src\TexFileTypeDialog.cs, src\BC4BC5Codec.cs, src\BC7Decoder.cs, src\BC7Encoder.cs, src\Bc7NativeInterop.cs, src\DirectXTexCompressor.cs, native\Bc7Native.cpp, Tools\Bc7ModeStats.csx). Line references are exact. **WARNING** flags mark correctness/reuse hazards. Completeness is prioritized over brevity.

> Note: There is **no `build.ps1`** in the repo (the task brief listed it, but it is absent). Native build is implied by the `.csproj` and `Bc7Native.cpp` comments only.

---

## 0. TL;DR for the rebuild (read this first)

This Paint.NET plugin is a **thin `.tex` container layer** (`TexFile.cs`) wrapping a set of **pure-C# BCn codecs that are line-by-line ports of Microsoft DirectXTex** (BC.cpp / BC4BC5.cpp / BC6HBC7.cpp), plus a **native `Bc7Native.dll`** that calls the real DirectXTex `Compress()` (GPU + CPU) for fast/high-quality BC7 encode.

The reusable backend splits cleanly:

1. **Container** — `TexFile.cs`. Riot `.tex` header (12 bytes), format byte enum, mip chain layout (smallest-first on disk), Lanczos3 mip generation. Pure `System.IO` arithmetic. **Note a real inconsistency:** the on-disk format byte values used by `TexFile` (BC7=13, BGRA8=20, BC5=14, RGBA16_SNORM=21) **conflict** with what `TexFile.MapFormat`-equivalent code and the two FileType layers assume — see §1.3 and §7. This must be pinned against real assets.
2. **Decoders** (all pure C#, portable to C++): BC1/DXT1 + BC3/DXT5 in `TexFile.cs` itself; BC5 (and BC4) in `BC4BC5Codec.cs`; BC7 full 8-mode decoder in `BC7Decoder.cs` (this is a complete, correct DirectXTex port — partition + fixup tables are FULLY populated here, unlike the GIMP sibling).
3. **Encoders:** BC1/BC3 via `DirectXTexCompressor.cs` (managed DirectXTex BC.cpp port with Floyd-Steinberg dithering + perceptual metric); BC5 via `BC4BC5Codec.cs` (managed); BC7 via `BC7Encoder.cs` (full managed DirectXTex BC7 encoder, all modes, parallel) **with a native fast path** through `Bc7NativeInterop` -> `Bc7Native.dll` -> DirectXTex `Compress` (GPU then CPU).
4. **Two parallel FileType front-ends:** `TexFileType` (simple, auto-detect) and `TexFileTypeDialog` (`PropertyBasedFileType` with format/dither/error-metric/mipmap UI). Both reuse the same `TexFile` + codecs.

**Best reuse target for Photoshop C++:** port `TexFile` container + the `BC7Decoder` tables/decode loop, and **link Microsoft DirectXTex directly** for encode (the C# encoders are faithful DirectXTex ports, so DirectXTex C++ gives identical-or-better results with far less code). `native\Bc7Native.cpp` is already a working C++/DirectXTex wrapper you can copy almost verbatim.

**Biggest caveat:** the format-byte enum is internally inconsistent across this codebase (§7.1). Do not trust any single table; validate against real `.tex` files and the LtMAO reference (README credits Tarngaina/LtMAO for the tex reading logic).

---

## 1. The `.tex` Binary Format (as implemented in `TexFile.cs`)

### 1.1 Constants and format codes (`TexFile.cs:15-20`)

```csharp
public const byte DXT1 = 10;
public const byte DXT5 = 12;
public const byte BC7  = 13;
public const byte BC5  = 14;
public const byte BGRA8 = 20;
public const byte RGBA16_SNORM = 21;
```

| Format byte | Const name | Meaning | Block / pixel size |
|------------:|-----------|---------|--------------------|
| 10 | `DXT1` | BC1 (RGB, opaque) | 8 bytes / 4x4 block |
| 12 | `DXT5` | BC3 (RGBA) | 16 bytes / 4x4 block |
| 13 | `BC7`  | BC7 (RGBA, high quality) | 16 bytes / 4x4 block |
| 14 | `BC5`  | BC5 (two-channel, normal maps) | 16 bytes / 4x4 block |
| 20 | `BGRA8` | uncompressed B8G8R8A8 | 4 bytes / pixel |
| 21 | `RGBA16_SNORM` | uncompressed 16-bit signed per channel | 8 bytes / pixel |

**Signature:** `0x00584554` (`TexFile.cs:58`, `:130`) = bytes `54 45 58 00` = ASCII `"TEX\0"`, stored/compared as a little-endian uint32.

### 1.2 Header byte-offset table (12 bytes)

Read in `TexFile.Read` (`TexFile.cs:52-104`), written in `TexFile.Write` (`TexFile.cs:125-241`). All multi-byte fields are little-endian (`BinaryReader`/`BinaryWriter` defaults).

| Offset | Size | Type | Field (read var) | Notes |
|-------:|-----:|------|------------------|-------|
| 0 | 4 | uint32 LE | signature | Must == `0x00584554`, else `throw FormatException` (`TexFile.cs:58-61`). |
| 4 | 2 | uint16 LE | `Width` | `TexFile.cs:65` |
| 6 | 2 | uint16 LE | `Height` | `TexFile.cs:66` |
| 8 | 1 | uint8 | `unknown1` | Read and discarded (`TexFile.cs:69`). Written as `1` (`:133`). |
| 9 | 1 | uint8 | `Format` | Format byte (§1.1). Read `:70`, written `:134`. |
| 10 | 1 | uint8 | `unknown2` | Read and discarded (`:71`). Written as `0` (`:135`). |
| 11 | 1 | uint8 (bool) | `Mipmaps` | `ReadBoolean` (`:72`): nonzero => has mip chain. Written via `bw.Write(Mipmaps)` (`:136`). |
| 12 | … | bytes | payload | Mip chain or single image (see §1.4). |

**Write order (`TexFile.cs:130-136`):**
```
bw.Write((uint)0x00584554); // off 0
bw.Write(Width);            // off 4 (ushort)
bw.Write(Height);           // off 6 (ushort)
bw.Write((byte)1);          // off 8  unknown1 = 1
bw.Write(Format);           // off 9
bw.Write((byte)0);          // off 10 unknown2 = 0
bw.Write(Mipmaps);          // off 11 bool (1 byte)
```

> **WARNING (write/read asymmetry):** On WRITE, byte 8 (`unknown1`) is set to `1`; byte 10 (`unknown2`) to `0`. On READ both bytes are ignored. `Tools\Bc7ModeStats.csx` documents the layout as `"4 sig, 2 width, 2 height, 1 unk, 1 format, 1 unk, 1 mipFlag"` and reads `format=data[9]`, `mips=data[11]` — consistent with the table above. So format lives at offset **9**, not 8.

### 1.3 Format-byte cross-reference (THE inconsistency — read carefully)

There are THREE different format-byte mappings in this single codebase:

1. **`TexFile.cs` constants** (authoritative for actual file I/O): DXT1=10, DXT5=12, **BC7=13**, **BC5=14**, **BGRA8=20**, RGBA16_SNORM=21.
2. **`TexFileTypeDialog.TexFileFormat` enum** (`TexFileTypeDialog.cs:257-264`): DXT1_BC1=10, DXT5_BC3=12, **BC7=13**, **BC5=14**, **BGRA8_Uncompressed=20**. Matches `TexFile`. Good.
3. **`TexFileType.cs`** (the SIMPLE FileType): uses `TexFile.DXT1/DXT5/BGRA8/BC5/BC7` constants throughout (e.g. `:142-149`, `:169-172`), so it agrees with `TexFile`.

So within THIS Paint.NET plugin the mapping is **internally consistent**: format byte 13 = BC7, 14 = BC5, 20 = BGRA8. (The earlier-audited GIMP sibling and/or my first draft used a *different* scheme — ignore that. The Paint.NET plugin's scheme is the one in the table above.)

**Reimplementation takeaway:** use {10:BC1, 12:BC3, 13:BC7, 14:BC5, 20:BGRA8, 21:RGBA16_SNORM}. Still validate against real Riot assets + LtMAO, because Riot's true codes are the ground truth and this is reverse-engineered.

### 1.4 Payload / mip layout

- **Block-compressed mip size** (`CalcMipSize`, `TexFile.cs:39-50`):
  ```
  if blockCompressed: bw=(w+3)/4, bh=(h+3)/4; size = bw*bh*GetBlockSize(fmt)
  if BGRA8:           size = w*h*4
  if RGBA16_SNORM:    size = w*h*8
  ```
  `GetBlockSize` (`:22-32`): DXT1=8, DXT5=16, BC5=16, BC7=16, else 0.
  `IsBlockCompressed` (`:34-37`): true for DXT1, DXT5, BC5, BC7.

- **Mip count** (`TexFile.cs:78-84` on read, `:141-148` on write): `maxDim = max(W,H); count = number of right-shifts until maxDim hits 0` = `floor(log2(maxDim)) + 1` (full chain down to 1x1). Read loop (`:78-84`): `mipmapCount=0; while(maxDim>0){mipmapCount++; maxDim>>=1;}`.

- **On-disk mip ORDER: SMALLEST FIRST.**
  - **Read** (`TexFile.cs:87-96`): loops `for i = mipmapCount-1 down to 0`, reading `mipWidth = max(W>>i,1)`, `mipHeight = max(H>>i,1)`. i = count-1 is the smallest level (e.g. 1x1), i = 0 is full-res. They are read in that order and appended to a list; **only the LAST entry read (i==0, the full-res level) is kept**: `tex.Data = mipmaps[mipmaps.Count - 1]` (`:96`). So on load, the plugin reads the whole chain but uses only level 0.
  - **Write** (`TexFile.cs:228-232`): generates levels full->small into `mipLevels` (index 0 = full), then writes `for i = mipLevels.Count-1 down to 0` — i.e. **smallest written first, largest last**. Matches the read order.

- **Non-mipmapped** (`TexFile.cs:98-104`): reads a single image of `CalcMipSize(fmt,W,H)` bytes; if format unknown, reads the remaining stream bytes.

### 1.5 Reimplementation skeleton (C++ pseudo-code)

```cpp
struct TexHeader { uint16_t w, h; uint8_t unk1, format, unk2; bool mips; };
// READ: u32 sig(==0x00584554), u16 w, u16 h, u8 unk1, u8 format, u8 unk2, u8 mipFlag
// format: 10=BC1, 12=BC3, 13=BC7, 14=BC5, 20=BGRA8, 21=RGBA16_SNORM
// payload @ off 12.
// If mips: chain stored SMALLEST-FIRST, count = floor(log2(max(w,h)))+1,
//          level i dims = max(w>>i,1) x max(h>>i,1).
// mipSize(BCn) = ceil(w/4)*ceil(h/4)*blockBytes; BGRA8 = w*h*4; RGBA16_SNORM = w*h*8.
// WRITE: unk1=1, unk2=0, mipFlag=mips. Largest level written LAST.
```

---

## 2. Load Pipeline (`.tex` -> Paint.NET Surface)

Two near-identical loaders: `TexFileType.OnLoad` (`TexFileType.cs:33-62`) and `TexFileTypeDialog.OnLoad` (`TexFileTypeDialog.cs:120-151`). Steps:

1. **Read whole stream** to `byte[] data` (`TexFileType.cs:35-36`).
2. **Parse + decode:** `TexFile.Read(data)` (§1) then `tex.DecompressToRgba()` (`:38-39`). Decompress returns **RGBA8** (R at byte 0), top-down, stride `W*4`.
3. **Dispatch** (`TexFile.DecompressToRgba`, `TexFile.cs:313-322`):
   | Format | Decoder | Source |
   |--------|---------|--------|
   | BGRA8 (20) | `DecompressBgra8` (swap B<->R) | `TexFile.cs:344-355` |
   | DXT1 (10) | `DecompressDxt1` | `:357-376`, block `:399-453` |
   | DXT5 (12) | `DecompressDxt5` | `:378-397`, block `:455-527` |
   | BC5 (14) | `BC4BC5Codec.DecompressBC5(.., isSigned:false)` | `BC4BC5Codec.cs:119-134` |
   | BC7 (13) | `BC7Decoder.DecompressBC7` | `BC7Decoder.cs:348-363` |
   | RGBA16_SNORM (21) | `DecompressRgba16Snorm` | `:324-342` |
   | else | `throw FormatException` | `:321` |
4. **Blit RGBA -> Surface** (`TexFileType.cs:44-56`): per pixel `surface[x,y] = ColorBgra.FromBgra(b, g, r, a)` where r/g/b/a come from the RGBA buffer indices 0/1/2/3. So the decoder emits RGBA, and the loader reorders into Paint.NET's BGRA `ColorBgra`. **No premultiply** (straight alpha).
5. **Store doc metadata:** the loaders stash `tex.Format` and `tex.Mipmaps` in static dictionaries keyed by `doc.GetHashCode()` (`TexFileType.cs:59-60`, `TexFileTypeDialog.cs:147-149`) so the simple OnSave can default to the original format. (Fragile — see §7.)

**Channel order summary (load):** file -> decoder output **RGBA** -> loader swaps to **BGRA** for `ColorBgra`. For the Photoshop port (which generally wants RGBA), you can skip the final swap and use the decoder output directly.

### 2.1 BC1/DXT1 decode detail (`TexFile.cs:399-453`)
- `color0/color1` = uint16 LE at block off / off+2; `colorBits` = uint32 LE at off+4.
- **RGB565->888 with bit replication** (`:415-420`): `r8=(r5<<3)|(r5>>2)`, `g8=(g6<<2)|(g6>>4)`, `b8=(b5<<3)|(b5>>2)`. (Higher quality than naive `*255/31`.)
- **Mode select by `color0 > color1`** (`:426`): 4-color opaque (2/3,1/3 interpolation) vs **3-color + transparent**: `colors[2]=avg`, `colors[3]={0,0,0,0}` (**真 transparent**, alpha 0 — `:434`). This loader correctly honors BC1 1-bit alpha.
- Index = `(colorBits >> (idx*2)) & 3` (`:444`), idx = `py*4+px`.

### 2.2 BC3/DXT5 decode detail (`TexFile.cs:455-527`)
- Alpha block first 8 bytes: `alpha0=data[off]`, `alpha1=data[off+1]`, `alphaBits` = 48-bit LE from off+2..off+7 (`:459-463`).
- Alpha palette (`:465-483`): 8-value if `alpha0>alpha1` (/7 interpolation), else 6-value (/5) + `alphas[6]=0, alphas[7]=255`. Indices = `(alphaBits>>(idx*3))&7`.
- Color block at off+8, **always 4-color mode** (correct for BC3 — `:504-508`).

### 2.3 RGBA16_SNORM decode (`TexFile.cs:324-342`)
- 4 channels x int16 LE per pixel. `-32768` clamped to `-32767`. Map `[-1,1] -> [0,255]` via `(s/32767 + 1)*0.5*255`. This is a display-only conversion (lossy to 8-bit).

---

## 3. Save Pipeline (Paint.NET Surface -> `.tex`)

### 3.1 Simple FileType (`TexFileType.OnSave`, `TexFileType.cs:64-175`)
1. **Flatten** to `scratchSurface` (`:66`).
2. **Pick format:** default DXT5 (12), overridden by the stored original format from the load-time dictionary (`:71-75`).
3. **Auto DXT1<->DXT5 swap by alpha content** (`:83-95`): scans all pixels; if a DXT1 doc now has alpha -> upgrade to DXT5; if a DXT5 doc is fully opaque -> downgrade to DXT1. (Silent, convenience behavior.)
4. **BGRA8 path** (`:99-115`): copy Surface to BGRA byte array (B,G,R,A), no compression.
5. **Block-compressed path** (`:116-150`):
   - **Hard requirement: width%4==0 && height%4==0**, else `throw FormatException` with a helpful message (`:118-126`). (League requires divisible-by-4.)
   - Copy Surface to **RGBA** byte array (`:128-140`).
   - Encode by format (`:142-149`):
     | Format | Encoder |
     |--------|---------|
     | DXT1 | `CompressDxt1Native` -> `DirectXTexCompressor.CompressBC1(rgba,w,h, dither=true, perceptual=true)` (`:177-181`) |
     | BC5 | `BC4BC5Codec.CompressBC5` |
     | BC7 | `BC7Encoder.CompressBC7` |
     | else (DXT5) | `CompressDxt5Native` -> `DirectXTexCompressor.CompressBC3(rgba,w,h,true,true)` (`:214-218`) |
6. **Mipmap flag** from dictionary (`:152-156`).
7. **Build `TexFile` + write** (`:158-174`): passes a compressor lambda to `TexFile.Write` so mip levels are compressed in the right format.

> **Dead code note:** `TexFileType` also contains an *unused* hand-rolled DXT block encoder (`CompressDxt1Block`/`CompressDxt5Block`/`CompressAlphaBlock`/`CompressColorBlockWithDithering`, `:183-551`). These are NOT called (the actual path uses `DirectXTexCompressor`). They are a self-contained alternate DXT encoder with the same Floyd-Steinberg + perceptual approach — usable as a simpler reference if desired, but the `DirectXTexCompressor` port is the live one.

### 3.2 Options FileType (`TexFileTypeDialog.OnSaveT`, `TexFileTypeDialog.cs:153-245`)
- Reads token props: `FileFormat`, `ErrorDiffusionDithering` (bool), `ErrorMetric` (Perceptual/Uniform), `GenerateMipMaps` (bool) (`:160-164`).
- `usePerceptual = (errorMetric == Perceptual)`.
- Same divisible-by-4 guard (`:189-199`).
- Encode dispatch (`:218-225`): DXT1 -> `CompressBC1(.., useDithering, usePerceptual)`; BC5 -> `CompressBC5`; BC7 -> `BC7Encoder.CompressBC7`; else DXT5 -> `CompressBC3(.., useDithering, usePerceptual)`.
- Passes `sourceRgba` (the full-res RGBA) into `TexFile.Write` so mip generation downsamples from the lossless source, not from decompressed data (`:216`, `:237-243`).

### 3.3 UI definition (`TexFileTypeDialog`, `:42-118`)
- `PropertyBasedFileType`. Properties: format dropdown (DXT1/DXT5/BC7/BC5/BGRA8, default DXT5), dithering checkbox (default true), error-metric radio (Perceptual default), mipmaps checkbox (default false).
- **Rules:** dithering + error-metric controls are read-only (disabled) unless format is DXT1 or DXT5 (`:52-66`) — those options only affect BC1/BC3 encode.

### 3.4 `TexFile.Write` + mip generation (`TexFile.cs:115-241`)
- Overloads: `Write()`, `Write(compressor)`, `Write(compressor, sourceRgba)` (`:110-125`). The compressor delegate signature is `Func<byte[] rgba, int w, int h, byte fmt, byte[]>`.
- Writes header (§1.2).
- **Mip branch** (`:138-233`) taken when `Mipmaps && format in {DXT1,DXT5,BC5,BC7,BGRA8}`:
  - Establish `currentRgba`: from `sourceRgba` if provided (lossless), else from BGRA `Data` converted to RGBA (BGRA8 case), else `DecompressToRgba()` fallback (`:151-173`).
  - Loop `mipmapCount` levels: for BGRA8 convert RGBA->BGRA per level; else call `compressor(mipRgba, mipW, mipH, Format)`; if no compressor, only level 0 uses raw `Data` then breaks (`:181-226`).
  - **Downsample between levels:** `DownsampleRgba` (`:257-311`) — **Lanczos3** (`a=3`) separable resampling with edge-clamped sample windows and weight normalization. Operates on RGBA floats, rounds to byte. (Much higher quality than a box filter; no gamma correction though.)
  - Write smallest-first (`:229-232`).
- **Non-mip branch** (`:234-237`): write `Data` as-is.

---

## 4. BCn Codecs (detailed)

### 4.1 BC1/BC3 decode — inline in `TexFile.cs`
Covered in §2.1/§2.2. Pure integer math, fully portable. BC1 correctly handles 1-bit transparency (unlike the GIMP sibling's BC1). Block reads are bounds-checked (`if blockIdx+8<=Data.Length`).

### 4.2 BC1/BC3 **encode** — `DirectXTexCompressor.cs`
A **faithful line-by-line port of DirectXTex `BC.cpp`** (header comment `:1-7` says "Exact line-by-line port of BC.cpp D3DXEncodeBC1"). This is the high-quality DXT encoder the README markets.

- **Perceptual luma weights** (`:21-25`): `LumR=0.2125/0.7154`, `LumG=1`, `LumB=0.0721/0.7154`, plus inverses. From DirectXTex BC.cpp.
- **`CompressBC1`** (`:27-65`): per 4x4 block, gather RGB floats, call `D3DXEncodeBC1` -> 8 bytes/block.
- **`CompressBC3`** (`:67-112`): per block, gather RGB + alpha; `EncodeBC3Alpha` (8 bytes) + `D3DXEncodeBC1` color (8 bytes) -> 16 bytes/block. Out-of-bounds pixels get alpha=255.
- **`D3DXEncodeBC1`** (`:115-394`):
  1. Optional **Floyd-Steinberg dithering** during RGB565 pre-quantization (`:140-183`) — full FS kernel (7/16, 3/16, 5/16, 1/16) respecting row boundaries.
  2. Apply perceptual luma after dither-error calc (`:185-191`).
  3. **`OptimizeRGB`** (`:397-560`): bounding box -> 4-direction axis test (swap G/B) -> Newton's-method endpoint refinement (8 iterations) — direct BC.cpp port.
  4. Encode endpoints to RGB565 (`Encode565` `:610-616`), handle degenerate equal-endpoint case (`:219-230`), determine swap, build 4-entry palette (`pSteps = {0,2,3,1}`), assign indices by projection with a second FS dithering pass (`:316-388`).
- **`EncodeBC3Alpha`** (`:562-606`): min/max alpha endpoints, 8-value (max>min, /7 with +3 rounding) or 6-value (/5 with +2) palette + {0,255}; nearest 3-bit indices packed into 48 bits.

**Quality:** This is genuine DirectXTex-grade BC1/BC3 with dithering and perceptual metric — the strongest part of this backend for DXT. **Portable to C++ verbatim, or just link DirectXTex.**

### 4.3 BC4/BC5 — `BC4BC5Codec.cs`
Header: "Based on Microsoft DirectXTex BC4BC5.cpp" (`:1-6`).

**Decode (`DecodeBC5` `:68-117`, `DecompressBC5` `:119-134`):**
- Block = two 8-byte BC4 sub-blocks: R at off, G at off+8. `dataR/dataG` read as uint64 LE.
- `DecodeUnormFromIndex` (`:18-36`) / `DecodeSnormFromIndex` (`:38-59`): standard BC4 6/8-value interpolation. SNORM clamps `-128->-127`.
- `GetIndex` (`:61-64`): `(data >> (3*offset + 16)) & 7` — note the **+16 bit offset** (endpoints occupy low 16 bits).
- **Normal-map blue reconstruction** (`:102-114`): treats R,G as normal X,Y; computes `Z = sqrt(1 - X^2 - Y^2)`, maps back to `[0,255]`. So BC5 decode outputs a usable RGB normal (B reconstructed), alpha=255. (The GIMP sibling zeroed blue; this one reconstructs it — better.)

**Encode (`CompressBC5` `:279-310`, `EncodeBC4UBlock` `:270-276`):**
- Per block, gather R and G as floats, encode two BC4-unorm sub-blocks.
- `FindEndPointsBC4U` (`:230-248`): bounding box + `b4` flag (0 or 1 present) selecting 6- vs 8-step `OptimizeAlpha`.
- `OptimizeAlpha` (`:151-228`): Newton's-method endpoint optimizer (DirectXTex `OptimizeAlpha<>` port), 6 or 8 steps.
- `FindClosestUNorm` (`:250-267`): build 8-entry gradient, nearest index, pack at `<< (3*i + 16)`.
- **No BC4 single-channel *file* path** (no format byte for BC4), but `EncodeBC4UBlock`/`DecodeUnorm` are reusable building blocks. There is also **no SNORM encoder** (decode only).

### 4.4 BC7 decode — `BC7Decoder.cs` (COMPLETE, correct)
Full 8-mode DirectXTex BC6HBC7.cpp port. **Unlike the GIMP sibling, the partition and fixup tables here are FULLY populated** (all 64 entries each), so this decoder works for every mode.

- **`ms_aInfo[8]`** (`:42-52`): per-mode `ModeInfo` (partitions, partitionBits, pBits, rotationBits, indexModeBits, indexPrec, indexPrec2, RGBA precisions with/without p-bit). Matches the BC7 spec.
- **`g_aPartitionTable`** (`InitPartitionTable` `:54-135`): `[regions-1][shape 0-63][pixel 0-15]`. 1-region all-zero; 2-region 64 shapes (`:62-96`); 3-region 64 shapes (`:99-133`). Complete.
- **`g_aFixUp`** (`InitFixUp` `:137-168`): anchor/fix-up indices per region count and shape. Complete (2-region `sec2` `:145-150`, 3-region `sec3` `:155-164`).
- **Weights** (`:19-21`): `g_aWeights2/3/4` standard BC7 interpolation weights.
- **Bit reader** `GetBit`/`GetBits` (`:178-204`): LSB-first within bytes, handles byte-straddling.
- **`DecodeBlock`** (`:229-346`):
  1. Mode = count leading zero bits until first 1 (`:234-236`); mode>=8 -> reserved -> transparent black (`:238-249`).
  2. Read shape/rotation/indexMode (`:259-261`).
  3. Read endpoints R,G,B,A (`:263-266`), p-bits (`:268-269`), apply p-bits (`:271-281`), unquantize (`:283-289`).
  4. Read primary indices (fix-up pixels get prec-1) (`:294-298`), secondary indices if `indexPrec2!=0` (`:300-307`).
  5. Interpolate per pixel (`InterpolateRGB`/`InterpolateA` `:212-226`), apply index-mode swap (`:316-329`), apply rotation channel swap (`:331-336`), store RGBA (`:341-344`).

**This is the single best artifact in the repo for the Photoshop decode path** — copy the tables + decode loop directly into C++.

### 4.5 BC7 encode — `BC7Encoder.cs` (full managed, all modes) + native fast path
Header: "BC7 Encoder (full, all 8 modes) - Pure C# Port ... BC6HBC7.cpp" (`:1-6`). This is a real, complete DirectXTex BC7 encoder, not a stub.

- **`CompressBC7`** (`:1054-1091`):
  1. **Try native first:** `Bc7NativeInterop.CompressBC7(rgba,w,h)`; if non-null, return it (`:1057-1058`). This is the DirectXTex GPU/CPU path.
  2. **Managed fallback:** `Parallel.For` over blocks, gather RGBA LDR+HDR, `EncodeBlock` per block (`:1061-1090`).
- **`EncodeBlock`** (`:983-1051`): iterates modes 0..7 (skips **modes 0 and 2**, the 3-subset modes, for speed `:1000`; skips mode 7 when no alpha `:1001`), iterates rotations/index-modes/shapes, computes `RoughMSE`, sorts shapes, `Refine`s the best `uItems = max(1, shapes>>2)`, keeps min-MSE block.
- Full machinery present: `GeneratePaletteQuantized`, `ComputeError`, `OptimizeRGB`/`OptimizeRGBA` (Newton's method), `RoughMSE`, `AssignIndices` (with fix-up endpoint swap), `FixEndpointPBits` (p-bit majority vote, shared p-bits for mode 1), `MapColors`/`PerturbOne`/`Exhaustive`/`OptimizeOne`/`OptimizeEndPoints` (endpoint refinement), `EmitBlock` (bit packing) (`:138-936`). All faithful DirectXTex ports.

**Quality:** genuine DirectXTex BC7 (minus 3-subset modes by default, matching DirectXTex without `BC_FLAGS_USE_3SUBSETS`). Far higher quality than a bounding-box encoder. Portable, but large — for C++, prefer linking DirectXTex directly (identical results).

### 4.6 Native BC7 — `native\Bc7Native.cpp` + `Bc7NativeInterop.cs`
**This is the real DirectXTex C++ wrapper** (not a hand-rolled mode-6 encoder). Best direct reuse for the Photoshop plugin.

**`Bc7Native.cpp` exports (C ABI, `__cdecl`, `__declspec(dllexport)`):**

| Signature | Source | Behavior |
|-----------|--------|----------|
| `int EncodeBC7ImageGPU(uint8_t* outBlocks, const uint8_t* inRgba, uint32_t w, uint32_t h, uint32_t flags)` | `:72-99` | Creates a D3D11 compute device (HW, WARP fallback), builds an R8G8B8A8 `ScratchImage`, calls DirectXTex `Compress(device, ..., DXGI_FORMAT_BC7_UNORM, flags, 1.0f, dst)`. Returns 1 on success, 0 to signal fallback. |
| `int EncodeBC7Image(uint8_t* outBlocks, const uint8_t* inRgba, uint32_t w, uint32_t h, uint32_t flags)` | `:101-124` | CPU path: DirectXTex `Compress(src, ..., BC7_UNORM, flags | TEX_COMPRESS_PARALLEL, TEX_THRESHOLD_DEFAULT, dst)`. |
| `int GpuAvailable()` | `:126-130` | 1 if a D3D11 compute device can be created. |

- Input: **RGBA8 row-major top-down** (`MakeSourceImage` `:23-34`, `DXGI_FORMAT_R8G8B8A8_UNORM`).
- Output: tightly packed BC7 blocks, 16 bytes/block, `CopyBlocksOut` strips row pitch (`:37-45`).
- **Runtime deps:** `DirectXTex.lib`/headers at build; at runtime needs `d3d11.dll`/`dxgi.dll` (system) for the GPU path. DirectXTex is statically linked into `Bc7Native.dll` (single DLL shipped per README).

**`Bc7NativeInterop.cs`:**
- `DllName = "Bc7Native.dll"` (`:14`), all `Cdecl`.
- P/Invoke: `EncodeBC7Image`, `EncodeBC7ImageGPU`, `GpuAvailable` (`:23-40`).
- Flag constants passed through: `TEX_COMPRESS_BC7_USE_3SUBSETS=0x80000`, `TEX_COMPRESS_BC7_QUICK=0x100000` (`:20-21`).
- `ProbeAvailable` (`:63-76`): finds the DLL next to the managed assembly and `NativeLibrary.Load`s it.
- `CompressBC7` (`:47-61`): if GPU available, try GPU; else/then CPU; returns null on total failure (-> managed fallback).

### 4.7 `DirectXTexCompressor.cs` — name clarification
**Despite the name, this class is NOT a wrapper around the native DirectXTex DLL.** It is the **pure managed C# port** of DirectXTex's BC1/BC3 (`BC.cpp`) encoder described in §4.2. The native DirectXTex usage lives only in `Bc7Native.cpp` (BC7) via `Bc7NativeInterop`. So:
- BC1/BC3 encode = always managed (`DirectXTexCompressor`).
- BC7 encode = native DirectXTex if `Bc7Native.dll` present (GPU>CPU), else managed `BC7Encoder`.
- BC5 encode = always managed (`BC4BC5Codec`).

---

## 5. DirectXTex dependency summary

| Path | What runs | Native dep? |
|------|-----------|-------------|
| BC1/BC3 encode | Managed C# port of DirectXTex BC.cpp (`DirectXTexCompressor.cs`) | No |
| BC5 encode/decode | Managed C# port of DirectXTex BC4BC5.cpp (`BC4BC5Codec.cs`) | No |
| BC7 decode | Managed C# port of DirectXTex BC6HBC7.cpp (`BC7Decoder.cs`) | No |
| BC7 encode (fast) | **Real DirectXTex `Compress()`** in `Bc7Native.dll` (GPU via D3D11 compute, or CPU parallel) | **Yes** — ships `Bc7Native.dll`; GPU path needs `d3d11.dll`/`dxgi.dll` |
| BC7 encode (fallback) | Managed C# port `BC7Encoder.cs` | No |

So the only *binary* dependency is the bundled `Bc7Native.dll`. Everything else is self-contained managed code. Per README, without `Bc7Native.dll` BC7 still works (managed fallback) but is slow.

---

## 6. Public surface worth reusing (for the C++/Adobe-SDK Photoshop plugin)

### 6.1 Pure-algorithm, portable to C++ (recommended)

| C# symbol | File:line | Reuse note |
|-----------|-----------|------------|
| Header read/write, format codes, `GetBlockSize`/`IsBlockCompressed`/`CalcMipSize` | `TexFile.cs:15-50, 52-104, 125-136` | Container core. ~60 lines of C++. |
| Mip count + smallest-first layout | `TexFile.cs:78-96, 228-232` | Critical to match Riot ordering. |
| `DownsampleRgba` (Lanczos3) | `TexFile.cs:246-311` | High-quality mip filter. Portable; consider gamma-correct variant. |
| BC1 decode | `TexFile.cs:399-453` | Correct 1-bit alpha. |
| BC3 decode | `TexFile.cs:455-527` | |
| RGBA16_SNORM decode | `TexFile.cs:324-342` | |
| BC5 decode (+ normal Z reconstruction) | `BC4BC5Codec.cs:68-134` | |
| BC5 encode | `BC4BC5Codec.cs:151-310` | |
| BC1/BC3 encode (DirectXTex port, dither+perceptual) | `DirectXTexCompressor.cs:27-624` | Or just link DirectXTex. |
| **BC7 decode (all modes, complete tables)** | `BC7Decoder.cs` (whole) | **Top reuse target.** Copy tables + decode loop. |
| BC7 encode (managed, all modes) | `BC7Encoder.cs` (whole) | Large; prefer DirectXTex link. |
| **`Bc7Native.cpp` DirectXTex wrapper** | `native\Bc7Native.cpp` (whole) | **Already C++.** Copy near-verbatim for BC7 encode (GPU+CPU). |

### 6.2 .NET-specific (reimplement with Adobe SDK equivalents)

| Symbol | File | Why |
|--------|------|-----|
| `TexFileType`, `TexFileTypeDialog` (OnLoad/OnSave/UI) | `TexFileType.cs`, `TexFileTypeDialog.cs` | Paint.NET `FileType`/`PropertyBasedFileType`, `Document`/`Surface`/`ColorBgra`, IndirectUI. Replace with Adobe File Format module callbacks + format-options dialog. |
| `Bc7NativeInterop` | `Bc7NativeInterop.cs` | P/Invoke. In native C++ call DirectXTex `Compress` directly (no interop). |
| `documentFormats`/`documentMipmaps` static dicts | both FileType files | Hash-keyed metadata hack; use the Adobe SDK's global/revert info instead. |

### 6.3 Recommended strategy
1. **Container:** port `TexFile` (header + mip math + Lanczos downsample) to C++ (~150 lines).
2. **Decode:** port `BC7Decoder` (complete), BC1/BC3 from `TexFile.cs`, BC5 from `BC4BC5Codec.cs`, RGBA16_SNORM. Output RGBA directly (skip the BGRA swap the Paint.NET loader does).
3. **Encode:** **link Microsoft DirectXTex** and reuse `Bc7Native.cpp`'s `Compress` calls for BC1/BC3/BC5/BC7 uniformly (the C# encoders are DirectXTex ports anyway, so this is simpler and identical/higher quality). Keep dither/perceptual via `TEX_COMPRESS_DITHER`/`TEX_COMPRESS_UNIFORM` flags.
4. **Channel order:** centralize one RGBA<->(whatever Adobe wants) conversion.
5. **Enforce divisible-by-4** for block formats (Riot requirement).

---

## 7. Limitations, bugs, TODOs, fragile assumptions

### 7.1 Header is reverse-engineered (validate against real assets) — HIGH
The 12-byte `TEX\0` header and the format-byte map are reverse-engineered (README credits LtMAO/Tarngaina for tex reading logic). Within the Paint.NET plugin the map is consistent ({13:BC7,14:BC5,20:BGRA8,21:RGBA16_SNORM}), but this **differs from other tools/audits** that use other numbering. **Pin the format codes against real Riot `.tex` files and LtMAO before shipping.** `unknown1`/`unknown2` (offsets 8 and 10) are written as fixed 1/0 but their real meaning is unknown — real files may carry data there (e.g. texture type / extended flags).

### 7.2 Write/read of unknown bytes is lossy
On load, `unknown1`/`unknown2` are discarded; on save they're forced to 1/0. If real files use those bytes meaningfully, round-tripping a real asset will corrupt them. Preserve original header bytes on round-trip in the rebuild.

### 7.3 Only mip level 0 used on load
`TexFile.Read` reads the entire mip chain but keeps only the full-res level (`TexFile.cs:96`). Fine for editing, but the smaller mips are parsed-then-discarded (minor waste; also means a corrupt small mip could still throw during read).

### 7.4 No stream-length validation on `ReadBytes`
`br.ReadBytes(mipSize)` (`TexFile.cs:92, 103`) can silently return fewer bytes on a truncated file; decoders then read a short/garbage buffer. Decoders DO bounds-check block offsets (e.g. `TexFile.cs:368`, `:389`, `BC4BC5Codec.cs:129`, `BC7Decoder.cs:358`), so this degrades to partial/black output rather than a crash, but no clear error is raised.

### 7.5 Document-metadata-by-hashcode is fragile
`documentFormats[doc.GetHashCode()]`/`documentMipmaps` (both FileType files) use `Document.GetHashCode()` as a key. Hash codes can collide and aren't stable identity; a new doc could pick up another doc's stored format, or lose it entirely (then defaults to DXT5). The options dialog avoids relying on it for format (uses the token) but still writes it. Don't replicate this pattern in the rebuild.

### 7.6 BC7 encoder skips 3-subset modes
`EncodeBlock` skips modes 0 and 2 (`BC7Encoder.cs:1000`) to save time (matches DirectXTex default). Slightly lower quality on some blocks; acceptable. The native DirectXTex path honors the same default unless `TEX_COMPRESS_BC7_USE_3SUBSETS` (0x80000) is passed (it isn't, by default).

### 7.7 Mip downsample has no gamma/sRGB awareness
`DownsampleRgba` (Lanczos3) averages raw 8-bit values (`TexFile.cs:257-311`). sRGB color mips will be slightly dark. Alpha is resampled with the same kernel (can cause ringing on hard alpha edges, since Lanczos has negative lobes — clamped to [0,255], so overshoot is clipped).

### 7.8 No premultiplied-alpha handling
All paths assume straight alpha. If real `.tex` data or Paint.NET surfaces ever use premultiplied alpha, colors will be wrong. (Likely fine — Riot `.tex` is straight alpha — but unverified.)

### 7.9 Dead/duplicate encoder code in `TexFileType.cs`
`TexFileType.cs:183-551` contains a complete *unused* hand-rolled DXT1/DXT5 encoder (with its own dithering/perceptual logic) that is never called (the live path uses `DirectXTexCompressor`). Confusing; could be deleted. Not a bug, but a reuse hazard (don't port the dead one).

### 7.10 Native GPU path swallows errors
`EncodeBC7ImageGPU` returns 0 on any failure and the interop silently falls to CPU then managed (`Bc7NativeInterop.cs:53-60`). No logging — a misbehaving GPU driver silently degrades to slow paths. Fine for robustness, invisible for diagnosis.

### 7.11 Platform / build assumptions
`.csproj` is `net9.0-windows`, x64, `AllowUnsafeBlocks`, WinForms, hardcoded `C:\Program Files\paint.net\` reference hints (`TexFileTypePlugin.csproj:14-17`). `Bc7Native.dll` is built separately (no build script in repo) and copied via the `<None>` item (`:58-63`). All Windows/x64; the *managed codec algorithms* are platform-neutral.

### 7.12 Licensing (reuse constraint) — important
`LICENSE.md` is a **custom copyleft "Open Source Requirement"** license (Copyright (c) 2025 budlibu500): attribution required AND any derivative must itself be open source / forkable / republishable; **proprietary/closed-source use is prohibited**. Meanwhile the codec algorithms are credited to Microsoft DirectXTex (MIT) and tex logic to LtMAO. **Implication:** verbatim reuse of THIS plugin's source imposes the budlibu500 copyleft on the Photoshop plugin. The underlying BCn algorithms are MIT (DirectXTex) and can be taken from upstream DirectXTex directly to avoid the copyleft. Decide the rebuild's license posture accordingly; safest is to base the C++ backend on upstream DirectXTex (MIT) + an independently re-derived `.tex` container.

---

## 8. File inventory audited

| File | Role | Key lines |
|------|------|-----------|
| `README.md` | Features, formats, install, credits (LtMAO, DirectXTex) | whole |
| `LICENSE.md` | Custom copyleft (budlibu500) | whole |
| `TexFileTypePlugin.csproj` | net9.0-windows x64, PDN refs, Bc7Native.dll copy | 1-65 |
| `.gitignore` | excludes native DLL/obj | whole |
| `src\TexFile.cs` | **container core** + BC1/BC3/BGRA8/SNORM decode + Lanczos mips | 1-529 |
| `src\TexFileType.cs` | simple FileType OnLoad/OnSave (+ dead DXT encoder) | 1-553 |
| `src\TexFileTypeDialog.cs` | PropertyBasedFileType w/ options UI | 1-272 |
| `src\BC4BC5Codec.cs` | BC4/BC5 decode + BC5 encode (DirectXTex port) | 1-312 |
| `src\BC7Decoder.cs` | **full BC7 decoder, complete tables** | 1-365 |
| `src\BC7Encoder.cs` | full managed BC7 encoder (all modes) + native fast path | 1-1093 |
| `src\Bc7NativeInterop.cs` | P/Invoke to Bc7Native.dll (GPU/CPU) | 1-78 |
| `src\DirectXTexCompressor.cs` | **managed** BC1/BC3 encoder (DirectXTex BC.cpp port) | 1-625 |
| `native\Bc7Native.cpp` | real DirectXTex BC7 wrapper (D3D11 GPU + CPU) | 1-132 |
| `Tools\Bc7ModeStats.csx` | BC7 mode-usage analyzer; confirms header offsets | 1-50 |

---

*End of audit.*
