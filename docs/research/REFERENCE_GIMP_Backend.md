# RitoTex GIMP Plugin — Reimplementation-Grade Technical Audit

Audit target: `e:\RitoShark\RitoTex\Gimp-Tex-Plugin`
Purpose: capture the GIMP plugin's complete understanding of Riot's `.tex` format and its load/save/compression machinery so a rebuilt Photoshop plugin and a shared format spec can be derived from it.

This audit is based on a full read of every source file. File:line references are to the files as they exist in the target folder.

---

## 0. Repository inventory

| Path | Lines | Role |
|------|-------|------|
| `README.md` | 137 | User-facing docs, format table, export options |
| `LICENSE` | 22 | MIT (Microsoft Corporation — inherited from DirectXTex) |
| `.gitignore` | 21 | Excludes `*.dll`, build artifacts, `*.log`, `installer/*.exe`, `release/` |
| `.github/workflows/build.yml` | 117 | CI: builds native lib for Win/Linux/macOS, builds installer + release zips |
| `gimp2/gimp2_tex_plugin.py` | 362 | GIMP 2.10 Python-Fu plugin (Python 2) |
| `gimp3/gimp3_tex_plugin.py` | 401 | GIMP 3.x GObject-Introspection plugin (Python 3) |
| `shared/tex_core.py` | 539 | **Core .tex logic** — parse/serialize, DDS conversion, mipmaps, decoders |
| `shared/dxt_compress.py` | 422 | ctypes DLL loader + pure-Python BC1/BC3 fallback compressor |
| `shared/dxt_compress.c` | 754 | Native BC1/BC3 encode+decode, BGRA swap, Lanczos3 downsample |
| `installer/GIMP_TEX_Plugin_Setup.iss` | 269 | Inno Setup installer (auto-detects GIMP 2.x/3.x) |

The compiled native library (`libdxtcompress.dll` / `.so` / `.dylib`) is **not** committed (gitignored); it is produced by CI and shipped in the releases/installer.

Architecture: both GIMP plugins are thin UI shells. **All format and codec knowledge lives in `shared/tex_core.py` and `shared/dxt_compress.{py,c}`.** This shared layer is the reusable core for any other host (Photoshop, Paint.NET, CLI).

---

## 1. The `.tex` binary format (as this plugin understands it)

Source of truth: `shared/tex_core.py` lines 7-17 (docstring), 25-37 (constants), 76-103 (parse/serialize).

### 1.1 Magic / signature

- Constant: `TEX_SIGNATURE = 0x00584554` (`tex_core.py:25`).
- Read as a little-endian `uint32` (`struct.unpack_from('<IHHBBBB', ...)`, `tex_core.py:81`).
- `0x00584554` little-endian on disk = bytes `54 45 58 00` = ASCII `"TEX\0"` (`T`=0x54, `E`=0x45, `X`=0x58, NUL=0x00).
- On a signature mismatch the parser raises `ValueError('Invalid TEX signature ...')` (`tex_core.py:82-83`).

### 1.2 Header layout — full byte-offset table (12 bytes total)

The header is a fixed 12-byte structure. Unpack format string is `'<IHHBBBB'` (`tex_core.py:81`, `tex_core.py:98`). `<` = little-endian for the whole struct.

| Offset | Size | Type | struct char | Field | Read value / meaning | Written value on export |
|--------|------|------|-------------|-------|----------------------|--------------------------|
| 0 | 4 | uint32 LE | `I` | `signature` | must equal `0x00584554` (`"TEX\0"`) | `0x00584554` (`tex_core.py:99`) |
| 4 | 2 | uint16 LE | `H` | `width` | texture width in pixels | `self.width` (`tex_core.py:100`) |
| 6 | 2 | uint16 LE | `H` | `height` | texture height in pixels | `self.height` (`tex_core.py:100`) |
| 8 | 1 | uint8 | `B` | `unknown1` | parsed into `unk1`, then **ignored**; doc says "always 1" | **hardcoded `1`** (`tex_core.py:101`) |
| 9 | 1 | uint8 | `B` | `format` | format enum (see §1.3); validated against `BLOCK_INFO` | `self.format` (`tex_core.py:101`) |
| 10 | 1 | uint8 | `B` | `unknown2` | parsed into `unk2`, then **ignored**; doc says "always 0" | **hardcoded `0`** (`tex_core.py:101`) |
| 11 | 1 | uint8 | `B` | `mipmaps` | `bool(mips)` — treated as a flag (0 = no mips, nonzero = has mips) | `1 if self.mipmaps else 0` (`tex_core.py:102`) |
| 12 | — | bytes | — | `data` | raw pixel/block payload, all mip levels concatenated | `self.data` appended after header (`tex_core.py:103`) |

Notes / fragile assumptions:
- **`unknown1` (offset 8) and `unknown2` (offset 10) are read but discarded.** They are not stored on the `TexFile` object (`__slots__` at `tex_core.py:59` only has `width, height, format, mipmaps, data`). On round-trip export they are **forced** to `1` and `0` respectively. If a real `.tex` carries a different value in either byte, that information is silently lost and overwritten.
- The `mipmaps` byte is interpreted as a **boolean flag**, not a mip count. The actual mip count is *derived* from dimensions (see §1.4), never read from the file.
- Minimum valid file size is 12 bytes (`tex_core.py:78-79`).

### 1.3 Format enum

Source: `tex_core.py:28-37`.

| Enum constant | Decimal | Hex | Meaning | DXT/BCn equivalent | Block size (px) | Bytes/block |
|---------------|---------|-----|---------|--------------------|-----------------|-------------|
| `FMT_DXT1` | 10 | 0x0A | DXT1 / BC1 (no alpha, or 1-bit punch-through) | BC1 | 4×4 | 8 |
| `FMT_DXT5` | 12 | 0x0C | DXT5 / BC3 (interpolated 8-bit alpha) | BC3 | 4×4 | 16 |
| `FMT_BGRA8` | 20 | 0x14 | Uncompressed 32-bit, **B,G,R,A byte order on disk** | (none) | 1×1 | 4 |

`BLOCK_INFO` maps each format to `(block_size, bytes_per_block)`:
```python
BLOCK_INFO = {
    FMT_DXT1: (4, 8),
    FMT_DXT5: (4, 16),
    FMT_BGRA8: (1, 4),
}
```
Any format byte not present in this dict is rejected at parse time (`tex_core.py:84-85`).

#### Formats NOT handled (important gaps vs. the real format)

This plugin only knows **three** format codes (10, 12, 20). The real Riot `.tex` format and other tools recognize more. The audit task explicitly asks about BC4/BC5/BC7 — **none of these are supported here**:

- **No BC4** (single-channel, format value typically used for grayscale/specular masks).
- **No BC5** (two-channel, normal maps).
- **No BC7** (high-quality RGBA block).
- **No ETC** or other codes.

The format value `0` (which in some Riot tooling can appear) is also unsupported. Cross-check against the Paint.NET plugin and LtMAO for the authoritative full enum — this GIMP plugin's enum is a **subset**.

#### Cross-check notes (DIFFERENCES from a standard DDS/DXT assumption)

These are the format details that deviate from "just treat it like a DDS":

1. **The `.tex` header is 12 bytes, custom, NOT a DDS header.** It is a Riot-proprietary container. The plugin synthesizes a DDS header on the fly only to reuse GIMP's DDS loader (see §2). A naive DDS reader pointed at a `.tex` will fail.
2. **Format codes are Riot enum values (10/12/20), not FourCC or DXGI codes.** `10`≠DXGI_BC1, it is Riot's own numbering. The mapping 10→DXT1, 12→DXT5, 20→BGRA8 is plugin/LtMAO convention.
3. **Mipmaps are stored SMALLEST-to-LARGEST** in `.tex` — the reverse of DDS, which stores largest-to-smallest. This is the single most important byte-order gotcha (see §1.4, §2.3, §3.4). DXT/DDS reimplementers will get garbage if they assume DDS ordering.
4. **The 32-bit uncompressed format is BGRA on disk, not RGBA.** Channel order swap is mandatory on both load and save (see §2.2, §3.3).
5. **The `mipmaps` field is a flag (0/1), not a count.** Mip count is computed from dimensions, so the file does not self-describe how many levels are present; the reader must trust the dimension-derived count exactly matches the payload length.
6. **`width`/`height` are `uint16`** → max texture dimension is 65535. (DDS uses uint32.)
7. **Two "unknown" bytes (offsets 8 and 10)** are part of the header. The plugin assumes 1 and 0 and discards/overwrites them. A faithful reimplementation should preserve them rather than hardcode.

### 1.4 Mipmap model

Source: `tex_core.py:105-141`.

- **Mip count is derived, never stored.** `mipmap_count()` (`tex_core.py:105-112`):
  ```python
  if not self.mipmaps: return 1
  max_dim = max(self.width, self.height)
  if max_dim == 0: return 1
  return max_dim.bit_length()
  ```
  `bit_length()` of the max dimension = number of mip levels down to 1×1. E.g. 1024 → `bit_length()` = 11 levels (1024, 512, ..., 1). Note: for a power-of-two N, `N.bit_length()` = log2(N)+1, which correctly counts down to 1×1.
- **Per-level sizing** — `mip_data_sizes()` (`tex_core.py:114-125`) returns `(w, h, byte_size)` per level, **largest first**:
  ```python
  w = max(self.width >> i, 1)
  h = max(self.height >> i, 1)
  bw = (w + block_size - 1) // block_size
  bh = (h + block_size - 1) // block_size
  byte_size = bw * bh * bytes_per_block
  ```
  This is standard block-count math. For BGRA8 (block_size 1, bpb 4) it reduces to `w*h*4`.
- **Storage order in the file: smallest-to-largest** (documented `tex_core.py:17`, `tex_core.py:129`). The largest mip is therefore at the **end** of the data blob.
- **Extracting the base (largest) image** — `get_largest_mip_data()` (`tex_core.py:127-141`): skips past all the smaller mips by summing their sizes, then slices out the final `largest_size` bytes. Used by the Python decompression fallback (it only ever decodes the base level; smaller mips are discarded on load).

Fragile assumption: the derived mip count must exactly match what is in the file. There is **no validation** that `len(data)` equals the sum of all level sizes. A `.tex` whose `mipmaps` flag is set but whose payload length doesn't match the computed chain will mis-slice (e.g. `get_largest_mip_data` returns a wrong offset). Likewise a file with `mipmaps=0` but extra trailing data is silently truncated to one level's worth on decode (block loops `break` on out-of-range — `tex_core.py:186-187`, `tex_core.py:232-233`).

---

## 2. Load pipeline (`.tex` → GIMP image)

There are two distinct load strategies. Both start by parsing the `.tex` header in `tex_core`, then differ in how they hand pixels to GIMP.

### 2.1 Common front-end

1. `TexFile.read(path)` (`tex_core.py:68-73`) reads the whole file into memory and calls `from_bytes`.
2. `from_bytes` (`tex_core.py:75-89`) validates size ≥ 12, unpacks the 12-byte header, validates signature and format, then stores `tex.data = raw[12:]` (the entire remaining blob, **all mips together**).

### 2.2 Primary strategy: TEX → synthesized DDS → GIMP's native DDS loader

This is the preferred path in BOTH plugins because GIMP ships a fast, native DDS importer that decodes BCn on the C side.

- `tex_to_dds_bytes(tex)` (`tex_core.py:283-376`) builds a complete DDS file in memory:
  - Picks the DDS pixel-format block per `.tex` format:
    - DXT1 → `DDPF_FOURCC`, FourCC `b'DXT1'` (`tex_core.py:291-295`).
    - DXT5 → `DDPF_FOURCC`, FourCC `b'DXT5'` (`tex_core.py:296-300`).
    - BGRA8 → `DDPF_RGB | DDPF_ALPHAPIXELS`, 32 bpp, masks **R=0x00FF0000, G=0x0000FF00, B=0x000000FF, A=0xFF000000** (`tex_core.py:301-308`). These masks describe **B,G,R,A byte order in memory** (little-endian), confirming the on-disk channel order is BGRA.
  - Sets DDS flags `DDSD_CAPS|DDSD_HEIGHT|DDSD_WIDTH|DDSD_PIXELFORMAT` and, if mipmapped, adds `DDSD_MIPMAPCOUNT` + caps `DDSCAPS_COMPLEX|DDSCAPS_MIPMAP` (`tex_core.py:313-317`). Comment notes this matches "Aventurine's DDS_HEADER_FLAGS_TEXTURE = 0x1007" (`tex_core.py:312`).
  - `dwPitchOrLinearSize`: for DXT, sets `DDSD_LINEARSIZE` and computes block-based linear size of the **base** level; for BGRA8 uses `width*4` (pitch) (`tex_core.py:319-325`).
  - Packs the 124-byte DDS_HEADER and asserts it is exactly 124 bytes (`tex_core.py:328-355`).
  - Prepends `DDS_MAGIC = 0x20534444` ("DDS ") → total 128-byte header (`tex_core.py:357-358`).
- **Mipmap reordering on load** (`tex_core.py:360-374`): because TEX stores small→large but DDS expects large→small, the code splits `tex.data` into chunks reading the file's small→large order, then writes them **reversed** (large→small) into the DDS body. For non-mipmapped textures it just appends `tex.data` verbatim.
- `tex_to_temp_dds(tex)` (`tex_core.py:379-388`) writes those DDS bytes to a `tempfile.mkstemp(suffix='.dds')` and returns the path. Caller deletes it.

GIMP 2.10 (`gimp2_tex_plugin.py:58-78`):
```python
dds_path = tex_to_temp_dds(tex)
image = pdb.file_dds_load(dds_path, dds_path, 0, 0)
pdb.gimp_image_set_filename(image, filename)
# finally: os.unlink(dds_path)
```
- Calls the DDS plugin via the PDB by name `file_dds_load`. Last two args `0, 0` are run-mode-ish/load flags. Then re-points the loaded image's filename at the original `.tex`.

GIMP 3.x (`gimp3_tex_plugin.py:97-120`):
```python
pdb_proc = Gimp.get_pdb().lookup_procedure('file-dds-load')
if pdb_proc is not None:
    dds_path = tex_to_temp_dds(tex)
    pdb_config = pdb_proc.create_config()
    pdb_config.set_property('run-mode', Gimp.RunMode.NONINTERACTIVE)
    pdb_config.set_property('file', dds_file)
    pdb_config.set_property('load-mipmaps', False)
    pdb_config.set_property('flip-image', False)
    result = pdb_proc.run(pdb_config)
    ...
```
- Uses the GIMP 3 config-object PDB call style. `load-mipmaps=False` (only base level imported), `flip-image=False` (no vertical flip — so **TEX rows are top-to-bottom, matching DDS-as-written and GIMP's expectation**; the plugin never flips).

Channel/byte order through this path: the DDS loader does the BCn decode and BGRA→display conversion internally, so the plugin doesn't touch pixels.

### 2.3 Fallback strategy: pure in-process decode (GIMP 3 only)

Only `gimp3_tex_plugin.py` has a real fallback (`gimp3_tex_plugin.py:122-137`). If `file-dds-load` is missing or fails:
```python
rgba = tex.decompress_to_rgba()
image = Gimp.Image.new(w, h, Gimp.ImageBaseType.RGB)
layer = Gimp.Layer.new(image, "Background", w, h, Gimp.ImageType.RGBA_IMAGE, 100.0, NORMAL)
image.insert_layer(layer, None, 0)
buffer = layer.get_buffer()
buffer.set(rect, "R'G'B'A u8", rgba)   # GEGL babl format: non-linear 8-bit RGBA
buffer.flush()
```
- `decompress_to_rgba()` (`tex_core.py:143-174`):
  1. Calls `get_largest_mip_data()` to isolate the base level.
  2. Tries `native_decompress` (the C DLL) first.
  3. Pure-Python fallback per format: BGRA8 → byte-swap to RGBA (`tex_core.py:158-165`), DXT1 → `_decompress_dxt1`, DXT5 → `_decompress_dxt5`.
- The fallback **only decodes the base mip**; mip data beyond base is ignored on load.
- GIMP 2.10 has **no** in-process fallback — if the DDS plugin call fails it just errors out.

### 2.4 Decoders (pure Python, in `tex_core`)

- `_decompress_dxt1` (`tex_core.py:177-220`): standard BC1. Reads `c0`,`c1` (RGB565 LE), expands 565→888 with bit-replication (`r0 = (r0<<3)|(r0>>2)` etc.). If `c0 > c1`: 4-color opaque mode (2 interpolated colors). Else: 3-color + transparent black (index 3 → RGBA 0,0,0,0). 2-bit index per texel, row-major within block.
- `_decompress_dxt5` (`tex_core.py:223-280`): BC3. Alpha block first (8 bytes): `a0`,`a1` endpoints + 48-bit (6-byte) index field, 3 bits per texel. If `a0 > a1`: 8-value interpolation; else 6 interpolated + explicit 0 and 255. Color block identical to BC1's 4-color mode (always 4-color for BC3). Output is full opaque RGB with per-texel interpolated alpha.
- Both produce **RGBA8** output (R,G,B,A byte order), matching GEGL's `"R'G'B'A u8"`.

The native C versions (`dxt_compress.c:536-586` `decompress_bc1`, `:592-658` `decompress_bc3`, `:664-673` `decompress_bgra8`) are byte-for-byte equivalent algorithms.

---

## 3. Save / export pipeline (GIMP image → `.tex`)

### 3.1 Format & option selection

Three user-controllable settings drive export:
- **Format**: index into `FORMAT_VALUES = [FMT_DXT1, FMT_DXT5, FMT_BGRA8]` (both plugins, e.g. `gimp3:41`). Default index 1 = DXT5.
- **Dithering** (bool): Floyd-Steinberg error diffusion during BC color quantization.
- **Error metric** (index): 0 = Perceptual (BT.709 luma weighting), 1 = Uniform. Maps to `perceptual = (metric_idx == 0)`.
- **Mipmaps** (bool): generate full Lanczos3 chain.

Settings persistence:
- GIMP 2: `gimp.set_data/get_data` with key `"gimp-tex-plugin-settings"`, packed as 4 bytes `BBBB` (`gimp2:237-253`).
- GIMP 3: a plain text file `~/.gimp_tex_export_settings`, comma-separated `fmt,dither,metric,mips` (`gimp3:44, 54-72`).
- Both default to `(1, True, 0, False)` = DXT5 + dithering + perceptual + no mips.

### 3.2 Two export entry points (both plugins)

- **Silent / quick export**: uses last-saved settings, no dialog. GIMP 2 `file-tex-save` (a real save handler, `gimp2:329-345`); GIMP 3 `file-tex-export` via `Gimp.ExportProcedure` (`gimp3:371-382`).
- **Export with options**: builds a GTK dialog (format combo, dither checkbox, metric combo, mip checkbox, with sensitivity wiring so metric only enables when dither+DXT) then a file chooser. GIMP 2 `file-tex-save-options` menu item (`gimp2:347-358, 94-234`); GIMP 3 `file-tex-export-options` via `Gimp.ImageProcedure` under `<Image>/File` (`gimp3:384-396, 173-289`).

### 3.3 Pixel extraction → compression → write

GIMP 2 (`gimp2:256-293` `_export_tex`):
1. `gimp_image_duplicate`, convert to RGB if needed, `gimp_image_merge_visible_layers(CLIP_TO_IMAGE)`, add alpha if layer isn't RGBA.
2. **Divisible-by-4 guard** for DXT: if `w%4 or h%4`, abort with a `gimp.message` telling the user the nearest valid size (`gimp2:273-281`).
3. `rgn = layer.get_pixel_rgn(0,0,w,h,False,False); rgba = bytearray(rgn[:, :])` — GIMP 2 pixel region, raw RGBA bytes.
4. `compressor = compress_for_tex(dither, perceptual)`; `tex = rgba_to_tex_data(rgba, w, h, fmt, mipmaps, compressor)`; `tex.write(filename)`.

GIMP 3 (`gimp3:296-342` `_do_export`):
1. `image.duplicate()`, `merge_visible_layers(CLIP_TO_IMAGE)`.
2. Same divisible-by-4 guard (returns `EXECUTION_ERROR` instead of a message box, `gimp3:311-318`).
3. `buffer.get(rect, 1.0, "R'G'B'A u8", Gegl.AbyssPolicy.CLAMP)` — pulls pixels in GEGL as non-linear 8-bit RGBA.
4. Same `compress_for_tex` → `rgba_to_tex_data` → `tex.write(path)`.

Both feed **RGBA8** into the shared core; the core handles all channel swapping and block encoding.

### 3.4 Core serialization — `rgba_to_tex_data` (`tex_core.py:391-426`)

- Validates compressor presence for DXT and divisible-by-4 again (defensive, `tex_core.py:406-414`).
- Non-mipmapped: `tex.data = _compress_level(rgba, w, h, fmt, compressor)` (single level).
- Mipmapped: `_generate_mipmap_chain` returns levels **largest→smallest**; the code stores them **reversed** (`b''.join(reversed(mip_levels))`) so the file ends up **smallest→largest** as required (`tex_core.py:421-424`). This is the write-side mirror of the load-side reordering.
- `_compress_level` (`tex_core.py:429-434`): BGRA8 → `_rgba_to_bgra` (channel swap); DXT → call the `compressor` callback.
- `_rgba_to_bgra` (`tex_core.py:437-451`): native DLL `rgba_to_bgra` if available, else pure-Python R↔B swap. Confirms **on-disk uncompressed payload is BGRA**.
- `_generate_mipmap_chain` (`tex_core.py:454-477`): iterates `mip_count = max(w,h).bit_length()` levels, compressing each then Lanczos3-downsampling RGBA to the next half-size (`max(dim//2,1)`).
- `TexFile.to_bytes()` then prepends the 12-byte header (§1.2).

### 3.5 When is the native C path used vs. pure Python?

`dxt_compress.py` tries to load the shared library **once** (`_init_dll`, `dxt_compress.py:55-126`) and caches the handle. Every operation (compress, decompress, bgra-swap, downsample) checks `_init_dll()`:
- **DLL present** → native (fast). e.g. `compress_bc1` → `_dll_compress_bc1` (`dxt_compress.py:193-200, 217-223`).
- **DLL missing/unloadable** → pure-Python fallback (functional but very slow): `_py_compress_bc1` / `_py_compress_bc3` (`dxt_compress.py:391-422`), plus pure-Python decode/downsample in `tex_core`.

So the native lib is an **optional accelerator**, never strictly required for correctness — the Python fallbacks implement the same DirectXTex algorithm. In practice the releases always ship the DLL, so the Python encoders are a safety net.

---

## 4. GIMP integration: 2.x vs 3.x

### 4.1 GIMP 2.10 (Python-Fu, `gimpfu`)

- API: `from gimpfu import *`, `import gimp`. This is the legacy Python 2 Python-Fu API.
- Pixel access: `layer.get_pixel_rgn(...)` and slice `rgn[:, :]` (`gimp2:283-284`). `bytearray(...)` is used specifically so indexing yields ints on Python 2 (comment `gimp2:284`).
- PDB calls: attribute style, e.g. `pdb.file_dds_load(...)`, `pdb.gimp_image_duplicate(...)`, `pdb.gimp_image_merge_visible_layers(...)`.
- Registration: three `register(...)` calls + `main()` at module load (`gimp2:311-361`):
  - `file-tex-load` with `menu="<Load>"` and `on_query=register_handlers` (which calls `gimp.register_load_handler("file-tex-load","tex","")`).
  - `file-tex-save` with `menu="<Save>"` (real save handler via `gimp.register_save_handler`).
  - `file-tex-save-options` as a plain menu item `<Image>/File/Export as .tex (options)...` (NOT a file handler).
- `register_handlers()` (`gimp2:300-308`) wires the load/save handlers to the `.tex` extension at query time.
- Logging: redirects `sys.stdout/stderr` to `~/gimp_tex_plugin.log` (`gimp2:33-39`).
- Module layout requirement: main `.py` sits flat in `plug-ins/`; shared libs go in a `gimp2_tex_libs/` subfolder added to `sys.path` (`gimp2:23-30`).

### 4.2 GIMP 3.x (GObject Introspection)

- API: `gi.require_version('Gimp','3.0')` etc., `from gi.repository import Gimp, GimpUi, Gtk, GObject, GLib, Gegl, Gio` (`gimp3:15-20`).
- Plugin is a **class** subclassing `Gimp.PlugIn` (`gimp3:349-398`):
  - `do_query_procedures()` returns `['file-tex-load','file-tex-export','file-tex-export-options']`.
  - `do_create_procedure(name)` builds the right procedure type:
    - load → `Gimp.LoadProcedure.new(...)`, `.set_extensions("tex")`.
    - export → `Gimp.ExportProcedure.new(...)`, `.set_extensions("tex")`.
    - options → `Gimp.ImageProcedure.new(...)`, `.add_menu_path("<Image>/File")`.
  - `do_set_i18n` returns `False` (no translations).
- Entry: `Gimp.main(TexPlugin.__gtype__, sys.argv)` (`gimp3:401`).
- Pixel access: GEGL buffers — `merged.get_buffer()`, `buffer.get(rect, 1.0, "R'G'B'A u8", AbyssPolicy.CLAMP)` on export; `buffer.set(rect, "R'G'B'A u8", rgba)` on the fallback load.
- Callback signatures differ: `load_tex(procedure, run_mode, file, metadata, flags, config, data)`; `export_tex(procedure, run_mode, image, file, options, metadata, config, data)`. Return values are `Gimp.ValueArray`/`new_return_values` with `Gimp.PDBStatusType`.
- Logging: `~/gimp_tex_plugin_3.log`.

### 4.3 Key API differences (2.x → 3.x) summary

| Concern | GIMP 2.10 | GIMP 3.x |
|---------|-----------|----------|
| Binding | `gimpfu` (Python 2) | PyGObject `gi` (Python 3) |
| Plugin shape | `register()` + `main()` at import | `Gimp.PlugIn` subclass + `Gimp.main()` |
| Procedure types | string menus `<Load>`/`<Save>` | `LoadProcedure` / `ExportProcedure` / `ImageProcedure` |
| Pixel I/O | `get_pixel_rgn` + numpy-ish slicing | GEGL `buffer.get/set` with babl format strings |
| File ext binding | `register_load/save_handler(...,"tex",...)` | `procedure.set_extensions("tex")` |
| PDB call | `pdb.file_dds_load(...)` | `lookup_procedure('file-dds-load')` + config object |
| Error reporting | `gimp.message(...)` | return `PDBStatusType.EXECUTION_ERROR` + `GLib.Error` |
| Settings store | `gimp.get_data/set_data` (binary) | text file in `$HOME` |
| In-process decode fallback | none | yes (`decompress_to_rgba`) |

### 4.4 Install locations (from README + installer)

- GIMP 2.x: `gimp2_tex_plugin.py` flat in `%APPDATA%\GIMP\<ver>\plug-ins\`, shared files in `…\plug-ins\gimp2_tex_libs\`.
- GIMP 3.x: everything inside `%APPDATA%\GIMP\<ver>\plug-ins\gimp3_tex_plugin\` (GIMP 3 requires a named subfolder matching the script).
- Linux/macOS equivalents listed in README §Installation; Flatpak path `~/.var/app/org.gimp.GIMP/config/GIMP/...`.

---

## 5. `dxt_compress.c` — native codec

### 5.1 Provenance & license

Header comment (`dxt_compress.c:1-15`): "Pure C port of Microsoft DirectXTex BC.cpp (MIT). **Ported from the C# implementation in Paint.NET-Tex-Plugin.**" So the lineage is DirectXTex (C++) → Paint.NET plugin (C#) → this (C). The `LICENSE` file is Microsoft's MIT, consistent with this.

### 5.2 Exported symbols (the ABI ctypes binds to)

`DLL_EXPORT` = `__declspec(dllexport)` on Windows, empty elsewhere (`dxt_compress.c:26-30`). Exports:

| C signature | Purpose | ctypes binding (`dxt_compress.py`) |
|-------------|---------|-------------------------------------|
| `void compress_bc1(const uint8_t *rgba, int w, int h, uint8_t *out, int dither, int perceptual)` | RGBA→BC1 | `:75-79` |
| `void compress_bc3(const uint8_t *rgba, int w, int h, uint8_t *out, int dither, int perceptual)` | RGBA→BC3 | `:83-87` |
| `void downsample_lanczos3(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)` | RGBA mip downsample | `:91-95` |
| `void rgba_to_bgra(const uint8_t *rgba, uint8_t *bgra, int num_pixels)` | channel swap | `:98-101` |
| `void decompress_bc1(const uint8_t *in, int w, int h, uint8_t *rgba)` | BC1→RGBA | `:104-107` |
| `void decompress_bc3(const uint8_t *in, int w, int h, uint8_t *rgba)` | BC3→RGBA | `:110-113` |
| `void decompress_bgra8(const uint8_t *in, int w, int h, uint8_t *rgba)` | BGRA→RGBA | `:116-119` |

All take `c_char_p` in/out buffers + `c_int`; Python allocates exact-size `ctypes.create_string_buffer` outputs sized by block math (`dxt_compress.py:217-232, 145-160, 163-190`).

### 5.3 How Python calls it (binding mechanism)

- **ctypes**, not cffi (`import ctypes`, `dxt_compress.py:13`).
- DLL discovery (`_find_dll`, `dxt_compress.py:35-53`): platform-specific name (`libdxtcompress.dll` / `.so` / `.dylib`), searched in the module's own directory then `os.getcwd()`.
- Loaded with `ctypes.CDLL(dll_path)` (cdecl), argtypes/restype declared once (`dxt_compress.py:71-122`). Single-attempt load guarded by `_dll_init_done`.
- Buffers passed as `bytes(...)`; results returned via `output.raw`.

### 5.4 Algorithm details (reusable knowledge)

- **Perceptual weights** (BT.709-derived, `dxt_compress.c:34-39`): `LUM_R = 0.2125/0.7154`, `LUM_G = 1.0`, `LUM_B = 0.0721/0.7154`, plus inverses. Colors are scaled into luma-weighted space before the optimizer and unscaled after (`encode_bc1_block`, `dxt_compress.c:292-296, 302-308, 326-332, 375-383`). Identical constants in Python (`dxt_compress.py:239-244`).
- **`optimize_rgb`** (`dxt_compress.c:93-241`): DirectXTex's endpoint optimizer — bounding box, 4-axis direction test (swaps G/B endpoints based on best axis), two-color shortcut, then up to 8 Newton's-method refinement iterations. Faithful port (Python twin `_optimize_rgb_py`, `dxt_compress.py:267-318`).
- **`encode_bc1_block`** (`dxt_compress.c:247-413`): 3-phase — (1) quantize to RGB565 with optional Floyd-Steinberg dithering, (2) optimize endpoints, (3) encode 2-bit indices with optional second dithering pass. Endpoint ordering: forces 4-color mode by swapping so `wA >= wB` (handles `wA==wB` degenerate by emitting zero index block). Uses `pSteps = {0,2,3,1}` index remap.
- **Floyd-Steinberg** (`propagate_error`, `dxt_compress.c:60-87`): classic 7/16, 3/16, 5/16, 1/16 weights distributed across the 4×4 block (edge-aware via `i & 3` checks). Python twin uses the same fractions written as decimals (`dxt_compress.py:257-265`).
- **`encode_bc3_alpha_block`** (`dxt_compress.c:419-458`): min/max alpha endpoints; if `max>min` 8-value interpolation, else 6 + explicit 0/255; nearest-palette 3-bit indices, packed into 48 bits.
- **`downsample_lanczos3`** (`dxt_compress.c:694-754`): `a=3` Lanczos kernel, separable-ish per-pixel gather with weight normalization, clamps to [0,255]. Uses `double` precision. Python twin `_downsample_lanczos3_pure` (`tex_core.py:500-539`) is identical math.

### 5.5 Portability / reusability for Photoshop

- **Highly portable.** Pure C99 (`stdint`, `math`), only `-lm` needed on POSIX. No DDS/TEX/GIMP knowledge inside — it operates purely on raw RGBA / BCn block buffers and width/height. This is exactly the kind of codec a Photoshop plugin (C/C++ or via the same ctypes/loadlibrary approach) can reuse unchanged.
- The TEX/DDS container logic lives entirely in Python (`tex_core.py`), so a Photoshop reimplementation can either reuse the C lib for blocks and reimplement the container, or port `tex_core.py`'s 12-byte header + mip-reversal logic directly.
- BC7/BC4/BC5 are absent — if the rebuilt plugin needs those, they must be added (the C file is a good template but covers only BC1/BC3).

---

## 6. Build & install

### 6.1 CI (`.github/workflows/build.yml`)

- Triggers: tags matching `v*` and manual `workflow_dispatch` (`build.yml:3-7`).
- **Build matrix** compiles the native lib on three runners (`build.yml:11-41`):
  - Windows: `cl /O2 /LD /Fe:libdxtcompress.dll dxt_compress.c` (MSVC, set up via `ilammy/msvc-dev-cmd`).
  - Linux: `gcc -shared -O3 -o libdxtcompress.so dxt_compress.c -lm`.
  - macOS: `gcc -dynamiclib -O3 -o libdxtcompress.dylib dxt_compress.c -lm`.
  - Each runs in `working-directory: shared`, uploads the lib as an artifact.
- **Release job** (`build.yml:43-117`, Windows runner):
  - Downloads all artifacts, `choco install innosetup`, runs `iscc installer\GIMP_TEX_Plugin_Setup.iss`.
  - Builds **6 release archives** (GIMP2/GIMP3 × Win/macOS/Linux) by assembling the right files into the right layout (GIMP3: flat folder; GIMP2: main script + `gimp2_tex_libs/` subfolder), zipping (Windows) or tar.gz (Unix), and `chmod +x` the Unix scripts.
  - On tag pushes, `softprops/action-gh-release` publishes the installer `.exe` + all 6 archives.

Note: the C build comment in source says `gcc` (`dxt_compress.c:14`) but CI actually uses MSVC `cl` on Windows; both work since the code is portable C with `_WIN32` guards.

### 6.2 Installer (`installer/GIMP_TEX_Plugin_Setup.iss`)

- Inno Setup, `PrivilegesRequired=lowest`, AppVersion 2.0.
- **Auto-detection** (`ScanForGIMP2`/`ScanForGIMP3`, `:49-121`): scans `%APPDATA%\GIMP\*` and `%LOCALAPPDATA%\GIMP\*` for directories starting `2.`/`3.` that contain a `plug-ins` subfolder; picks the highest version string. Errors out if neither found (`:148-159`).
- **Files installed** (`:24-38`):
  - GIMP 2: `gimp2_tex_plugin.py` into `<plug-ins>`; `tex_core.py`, `dxt_compress.py`, `libdxtcompress.dll` into `<plug-ins>\gimp2_tex_libs`.
  - GIMP 3: `gimp3_tex_plugin.py`, `tex_core.py`, `dxt_compress.py`, `libdxtcompress.dll` all into `<plug-ins>\gimp3_tex_plugin`.
  - **Windows installer ships only the `.dll`** (no `.so`/`.dylib` — correct for Windows).
- **Post-install** (`CurStepChanged`, `:216-249`): deletes `pluginrc` cache so GIMP re-scans plugins; shows a completion message.
- **Uninstall** (`:254-264`): removes plugin files for hardcoded versions (GIMP 2.8/2.10; GIMP 3.0/3.2/3.4/3.6) — note these are a **fixed list**, so a future version dir (e.g. 3.8) installed-to via auto-detect would not be cleaned by uninstall.

---

## 7. Limitations, bugs, fragile assumptions, TODOs

### 7.1 Format coverage gaps
1. **Only 3 formats (DXT1=10, DXT5=12, BGRA8=20).** No BC4/BC5/BC7, no other Riot enum values, no format `0`. Loading any other `.tex` raises `ValueError`. This is the biggest limitation for a faithful reimplementation.
2. **`unknown1`/`unknown2` header bytes are discarded and hardcoded to 1/0 on write** (`tex_core.py:101`). Real files with other values lose data on round-trip. A robust reborn implementation should round-trip these.
3. **`mipmaps` is a flag, mip count is derived from dimensions.** No validation that payload length matches the derived chain; mismatched files mis-slice silently.

### 7.2 Correctness / robustness
4. **No payload length validation** anywhere. `from_bytes` accepts any blob ≥ 12 bytes. Truncated DXT data just `break`s mid-decode leaving zero-filled pixels (`tex_core.py:186-187, 232-233`).
5. **NPOT mipmaps are technically generated but questionable.** `_generate_mipmap_chain` uses `dim//2` and `bit_length()` of the max dim; for non-power-of-two textures the per-level sizes (`>> i`) and the chain length can diverge from what the game expects, and the divisible-by-4 guard only checks the **base** level — intermediate DXT mip levels can become non-multiple-of-4 (e.g. 12→6→3) yet are still block-compressed via `(w+3)//4` padding. Whether the LoL engine accepts that padded layout is unverified.
6. **DXT requires width AND height divisible by 4** (guarded in plugins and core). No automatic resize/pad offered to the user — export just refuses. DDS/BCn normally allows arbitrary sizes with padded final blocks; this plugin is stricter than the format technically requires.
7. **BGRA8 fallback decode loop bug-prone bound**: `for i in range(0, len(data) - 3, 4)` (`tex_core.py:160`) writes into `rgba` indexed by `i` — this assumes `len(data) == w*h*4` exactly; if `data` is larger (extra mips with `mipmaps` flag but BGRA path) `get_largest_mip_data` should have trimmed it, but there is no cross-check.
8. **GIMP 2 has no in-process decode fallback** — if `file_dds_load` is unavailable the load fails entirely. Only GIMP 3 degrades gracefully.
9. **Native vs Python output may differ slightly** at the rounding/float-precision level (C uses `float` in the optimizer, Python uses `float`=C double in places). Cosmetic, but exact byte-reproducibility between the two encoders is not guaranteed.

### 7.3 Operational / packaging
10. **DLL discovery is directory-limited** (`module dir` + `cwd` only, `dxt_compress.py:44-47`). If the lib lands elsewhere it silently falls back to the (very slow) pure-Python encoder. The `_log` messages are the only diagnostic.
11. **`sys.stdout`/`sys.stderr` are globally redirected to a log file at import** (both plugins). This can swallow other plugins' console output in the same interpreter and is a side effect at module load.
12. **Installer uninstall uses a hardcoded version list** (2.8/2.10/3.0/3.2/3.4/3.6). Auto-detect can install into a newer dir that uninstall won't remove.
13. **Temp DDS file** is written to disk on every load (`tex_to_temp_dds`) and deleted in a `finally`. On a crash between write and unlink it leaks a temp `.dds`.
14. **Help URL points to `LeagueToolkit/Gimp-Tex-Plugin`** while attribution credits LtMAO — the repo home/branding is slightly inconsistent across files.

### 7.4 Things that are correct and worth carrying forward
- The **TEX↔DDS mipmap reversal** is handled consistently on both load and save (small↔large). This is the trickiest part of the format and is done right.
- The **BGRA-on-disk** handling (swap on both directions) is consistent and matches the DDS masks emitted.
- The **codec core is host-agnostic** and directly reusable for Photoshop.
- The **12-byte header pack/unpack** (`'<IHHBBBB'`) is the single canonical definition to copy into a shared spec.

---

## 8. Quick reference for the Photoshop rebuild

- **Header (12 bytes, little-endian):** `uint32 magic=0x00584554 ("TEX\0")`, `uint16 width`, `uint16 height`, `uint8 unk1(=1)`, `uint8 format`, `uint8 unk2(=0)`, `uint8 mipFlag`, then payload.
- **Format enum:** 10=DXT1/BC1, 12=DXT5/BC3, 20=BGRA8 (uncompressed, B,G,R,A on disk). (Add BC4/BC5/BC7/others as needed — not present here.)
- **Mips:** flag-only; count = `max(w,h).bit_length()`; stored **smallest→largest**; per-level size = `ceil(w/4)*ceil(h/4)*bpb` (DXT) or `w*h*4` (BGRA8).
- **DXT export constraint as implemented:** w%4==0 and h%4==0.
- **Codec:** reuse `dxt_compress.c` (BC1/BC3 encode+decode, BGRA swap, Lanczos3) verbatim; it's standalone C with a clean ctypes-friendly ABI.
- **Round-trip caution:** preserve the two "unknown" header bytes rather than hardcoding 1/0 (this plugin does not).

---

### 3-bullet summary

- **Format core lives in `shared/tex_core.py`**: a 12-byte little-endian header (`'<IHHBBBB'`) — magic `0x00584554` ("TEX\0"), uint16 w/h, an "unknown=1" byte, a 1-byte format enum (**10=DXT1, 12=DXT5, 20=BGRA8 only** — no BC4/BC5/BC7), an "unknown=0" byte, and a 1-byte mipmap *flag* — followed by all mip levels concatenated. The codec (`dxt_compress.c`) is a standalone, host-agnostic DirectXTex C port (BC1/BC3 + Lanczos3) that the Photoshop rebuild can reuse directly; the two GIMP scripts are thin UI shells (GIMP 2 = `gimpfu`/Python2/`register()`, GIMP 3 = `Gimp.PlugIn` subclass/GEGL).
- **Key DIFFERENCES from a standard DXT/DDS assumption:** (1) mipmaps are stored **smallest-to-largest**, the reverse of DDS — the plugin explicitly reverses on both load and save; (2) the 32-bit uncompressed format is **BGRA on disk**, not RGBA, requiring a channel swap each way; (3) format codes are Riot's own enum (10/12/20), not FourCC/DXGI; (4) the header carries **two "unknown" bytes** (offsets 8 and 10) that the plugin reads-then-discards and **hardcodes to 1 and 0 on write**, silently losing them on round-trip; (5) the mipmap field is a **boolean flag**, with mip count derived from `max(w,h).bit_length()` rather than stored.
- **Reborn watch-outs:** the format enum is a strict subset (no BC4/BC5/BC7, no format 0), there is **no payload-length validation**, DXT export refuses non-multiple-of-4 dimensions (no auto-pad), GIMP 2 has no in-process decode fallback, and the native lib is an optional ctypes accelerator (`libdxtcompress.{dll,so,dylib}`, built by CI, found only in the module dir or cwd) with slow pure-Python fallbacks for every operation.
