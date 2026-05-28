# GPU Block-Compression Plan (RitoTex Photoshop plugin)

**Goal:** Move slow CPU BC compression (especially BC7) onto the GPU using DirectXTex's
built-in DirectCompute compressor, with a clean CPU fallback. Research only — no code
shipped yet. This doc is the implementer's spec.

**Status of the idea:** Viable and low-effort. The bundled DirectXTex already ships and
already *compiles* the GPU compressor. We only add an `ID3D11Device`, route BC7 to the GPU
`Compress()` overload, and link `d3d11.lib`. Everything else stays as-is.

---

## 1. What DirectXTex already gives us

### 1.1 The GPU `Compress()` overloads (already declared)
From `3rdParty\DirectXTex\DirectXTex\DirectXTex.h:543-547`:

```cpp
HRESULT __cdecl Compress( _In_ ID3D11Device* pDevice, _In_ const Image& srcImage,
                          _In_ DXGI_FORMAT format, _In_ DWORD compress,
                          _In_ float alphaWeight, _Out_ ScratchImage& image );

HRESULT __cdecl Compress( _In_ ID3D11Device* pDevice, _In_reads_(nimages) const Image* srcImages,
                          _In_ size_t nimages, _In_ const TexMetadata& metadata,
                          _In_ DXGI_FORMAT format, _In_ DWORD compress,
                          _In_ float alphaWeight, _Out_ ScratchImage& cImages );
// DirectCompute-based compression (alphaWeight is only used by BC7. 1.0 is the typical value).
```

The **second** overload (Image\* + count + TexMetadata) is the drop-in twin of the CPU
overload already called in `Codec.cpp:52`. It handles the **entire mip chain and all array
slices (including 6-face cubes) internally** — see `DirectXTexCompressGPU.cpp:242-400`
(it loops `metadata.mipLevels` x `metadata.arraySize`, halving w/h each level, and calls
`gpubc->Prepare`/`_GPUCompress` per image). So no per-mip loop is needed on our side.

`alphaWeight` maps to the CPU call's `alphaRef` arg position; pass **1.0f** for the GPU
overload (BC7 weight), vs 0.5f used for the CPU `alphaRef` (BC1 cutoff). Different meaning,
different value.

### 1.2 Which formats run on GPU vs stay on CPU — IMPORTANT
The GPU compressor (`BCDirectCompute`) only supports **BC6H and BC7**. Confirmed in
`BCDirectCompute.cpp:202-224` — `GPUCompressBC::Prepare()` `switch(format)` handles only:

| DXGI format | GPU? | Notes |
|---|---|---|
| `BC6H_UF16/SF16/TYPELESS` | yes | source converted to RGBAF32. **Not used by this plugin.** |
| `BC7_UNORM`, `BC7_TYPELESS` | **yes** | source = `R8G8B8A8_UNORM` |
| `BC7_UNORM_SRGB` | **yes** | source = `R8G8B8A8_UNORM_SRGB` |
| anything else (BC1/BC2/BC3/BC4/BC5) | **NO** | `default:` -> `ERROR_NOT_SUPPORTED` |

So of the formats this plugin emits (`BC1_UNORM`, `BC3_UNORM`, `BC5_UNORM`, `BC7_UNORM`;
see `SaveOptionsDialog.cpp:197-209`):

- **BC7 -> GPU** (this is the big win; BC7 is the slow CPU path today).
- **BC1 / BC3 -> stay on ISPC** (already fast; GPU unsupported anyway).
- **BC1/BC3 with dithering -> stay on CPU `Compress()`** (GPU has no dither path).
- **BC5 -> stay on CPU `Compress()`** (GPU unsupported; BC5 is relatively cheap).
- Uncompressed RGBA8 -> unchanged.

Net: the *only* format we reroute is **BC7** (both UNORM and, if/when added, UNORM_SRGB).

### 1.3 Sources are already compiled
The plugin pulls DirectXTex via a **ProjectReference** to
`DirectXTex_Desktop_2012.vcxproj` (`IntelTextureWorks.vcxproj:424-426`). That project
already compiles the GPU pieces (`DirectXTex_Desktop_2012.vcxproj:381,388,390,392`):
`BCDirectCompute.h/.cpp`, `DirectXTexCompressGPU.cpp`, `DirectXTexD3D11.cpp`, plus the
precompiled compute shaders in `Shaders\Compiled\*.inc`. **No DirectXTex project changes
needed.** We only need our own plugin to (a) create a device and (b) link `d3d11.lib`.

---

## 2. Device creation (`D3D11CreateDevice`)

Pattern mirrors the proven Paint.NET `Bc7Native.cpp` wrapper (hardware first, WARP
fallback) documented in `docs\research\REFERENCE_PaintNET_Backend.md:280-282,310`.

```cpp
#include <d3d11.h>          // add to IntelPlugin.h alongside <dxgiformat.h>/<directxtex.h>
#include <wrl/client.h>     // Microsoft::WRL::ComPtr (already used by DirectXTex)

// Returns nullptr on failure (caller must fall back to CPU Compress()).
static HRESULT CreateCompressDevice(Microsoft::WRL::ComPtr<ID3D11Device>& device)
{
    UINT createFlags = 0;
#ifdef _DEBUG
    // Optional; only if the D3D11 SDK debug layer is installed, else it fails the create.
    // createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Feature level 11.0 is required for the BC7 compute shaders (cs_5_0).
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    // Try hardware first.
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
        levels, _countof(levels), D3D11_SDK_VERSION,
        device.ReleaseAndGetAddressOf(), nullptr, nullptr);

    if (FAILED(hr))
    {
        // Fall back to the WARP software rasterizer (headless / no GPU / RDP).
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createFlags,
            levels, _countof(levels), D3D11_SDK_VERSION,
            device.ReleaseAndGetAddressOf(), nullptr, nullptr);
    }
    return hr;   // device is non-null only on S_OK
}
```

Notes:
- `D3D11CreateDevice` pulls in `dxgi.dll` transitively; you only need to **link
  `d3d11.lib`** (DXGI is loaded at runtime, no `dxgi.lib` required for this call). Add
  `dxgi.lib` only if you later enumerate adapters with `CreateDXGIFactory`.
- Feature level **11_0 minimum** — the BC6H/BC7 encoders use `cs_5_0` compute shaders.
- WARP fallback means even on a headless/no-GPU box you still get a (slower-than-HW but
  often faster-than-our-scalar-CPU, and definitely *correct*) device. If even WARP fails,
  fall back to CPU `Compress()`.

### 2.1 Where it lives in `Codec.cpp` (create once, reuse, thread-safe)
- Create the device **lazily on first BC7 encode** and **cache it** (don't recreate per
  image — `GPUCompressBC::Initialize` recompiles/loads shaders, and the chain overload
  already reuses one `GPUCompressBC` across the whole mip chain).
- Recommended: a file-local helper in `Codec.cpp`:
  ```cpp
  // Codec.cpp (anonymous namespace)
  static Microsoft::WRL::ComPtr<ID3D11Device> g_compressDevice;
  static bool g_deviceTried = false;          // guard so we only attempt once
  static std::mutex g_deviceMutex;            // compression may run off main thread

  static ID3D11Device* GetCompressDevice() {
      std::lock_guard<std::mutex> lock(g_deviceMutex);
      if (!g_deviceTried) {
          g_deviceTried = true;
          CreateCompressDevice(g_compressDevice);   // leaves null on failure
      }
      return g_compressDevice.Get();
  }
  ```
- **Thread-safety caveat:** an `ID3D11Device` is free-threaded for *resource creation*, but
  the immediate `ID3D11DeviceContext` used internally by `GPUCompressBC` is **not**.
  DirectXTex's GPU `Compress()` creates a fresh `GPUCompressBC` (and grabs the device's
  immediate context) on every call. If two RitoTex compress operations can run
  concurrently on different threads sharing one device, serialize the actual `Compress()`
  call with the same mutex (or give each thread its own device). Simplest correct option:
  hold `g_deviceMutex` around the GPU `Compress()` call too. RitoTex compresses one export
  at a time today, so contention is effectively nil — the mutex is cheap insurance.
- Lifetime: a `ComPtr` static releases at DLL unload. That is fine for an `.8bi` plugin.
  (If shutdown-order paranoia arises, release it in the plugin's terminate path instead.)

---

## 3. Exact change to `Codec.cpp` dispatch

Current relevant block: `Codec.cpp:29-72` (the `useDirectXTex` branch that calls the CPU
`Compress()` at line 52).

Add a GPU sub-branch **inside** the existing `useDirectXTex` path, gated on BC7 + a
successfully created device. Pseudocode (keep existing error handling/messages):

```cpp
if (useDirectXTex)
{
    DWORD flags = 0;
    if (ps.data->useDithering && ispcCanEncode) {           // dither only on BC1/BC3
        flags |= TEX_COMPRESS_DITHER;
        if (ps.data->useUniformMetric) flags |= TEX_COMPRESS_UNIFORM;
    }

    const bool gpuEligible =
        (ps.data->encoding_g == DXGI_FORMAT_BC7_UNORM ||
         ps.data->encoding_g == DXGI_FORMAT_BC7_UNORM_SRGB);   // BC7 only; dither N/A for BC7

    ID3D11Device* dev = gpuEligible ? GetCompressDevice() : nullptr;

    (*scrImageScratch_)->Release();   // Compress() initializes the output internally

    HRESULT hr = E_FAIL;
    if (dev) {
        // GPU path — alphaWeight = 1.0f (BC7). Whole mip chain / cube handled internally.
        // std::lock_guard<std::mutex> lock(g_deviceMutex);   // if concurrent compresses possible
        hr = Compress(dev,
                      (*scrUncompressedImageScratch_)->GetImages(),
                      (*scrUncompressedImageScratch_)->GetImageCount(),
                      (*scrUncompressedImageScratch_)->GetMetadata(),
                      ps.data->encoding_g, flags, 1.0f,
                      **scrImageScratch_);
    }

    if (!dev || FAILED(hr)) {
        // CPU fallback (also the path for BC5 and dithered BC1/BC3): existing call, alphaRef 0.5f
        hr = Compress(
            (*scrUncompressedImageScratch_)->GetImages(),
            (*scrUncompressedImageScratch_)->GetImageCount(),
            (*scrUncompressedImageScratch_)->GetMetadata(),
            ps.data->encoding_g, flags, 0.5f,
            **scrImageScratch_);
    }

    if (FAILED(hr)) { /* existing errMsg + UserError + return false */ }
    return true;
}
```

Key points:
- **Only BC7 is `gpuEligible`.** BC5 and dithered BC1/BC3 never set `dev`, so they keep
  going through CPU `Compress()` exactly as today.
- **Fallback is automatic** on (a) device-creation failure (`dev == nullptr`) or (b) any
  `FAILED(hr)` from the GPU call (e.g. an odd driver). Same output `ScratchImage`, same
  downstream `.dds`/`.tex` writers — no other code changes.
- Optionally pass `TEX_COMPRESS_BC7_USE_3SUBSETS` for higher quality (slower); DirectXTex
  default (and PNet) omit it. Match current CPU behavior (omitted) to keep output parity.

---

## 4. vcxproj changes (`IntelCompressionPlugin\IntelTextureWorks.vcxproj`)

Only the plugin project needs edits (DirectXTex project is already correct). Apply to
**all four `<Link>` blocks** (Debug|Win32, Release|Win32, Debug|x64, Release|x64) — lines
`126`, `175`, `228`, `285`. Add `d3d11.lib` to `AdditionalDependencies`:

```
<AdditionalDependencies>d3d11.lib;odbc32.lib;odbccp32.lib;version.lib;winhttp.lib;shell32.lib;%(AdditionalDependencies)</AdditionalDependencies>
```

- `d3d11.lib` ships with the Windows SDK (already on the include path via
  `WindowsTargetPlatformVersion=10.0`). No new SDK install.
- `dxgi.lib` is **not** required for `D3D11CreateDevice` (it loads DXGI at runtime). Add it
  only if you later call DXGI factory APIs.
- No new `.cpp` files to add to the plugin (device helper lives in existing `Codec.cpp`).
  The GPU DirectXTex sources are compiled by the referenced DirectXTex project already.
- Add `#include <d3d11.h>` (and `<wrl/client.h>`, `<mutex>`) — cleanest in `IntelPlugin.h`
  right after the existing `#include <dxgiformat.h>` / `<directxtex.h>` (line 27-29), since
  `Codec.cpp` includes `IntelPlugin.h`.

> Win32 configs build the GPU code too (it's portable), so adding `d3d11.lib` to all four
> keeps things uniform. Shipping target is x64 (`README`/build), but don't break Win32.

---

## 5. Risks / caveats / verification

- **Headless / batch / no GPU:** Photoshop actions/Image Processor may run where no HW
  device exists (RDP, headless render box, locked-down GPU). The HW->WARP->CPU fallback
  chain covers all three; nothing errors out, worst case is the current CPU speed.
- **Photoshop is already using the GPU:** PS uses its own D3D/GL context for the canvas.
  Creating a *separate* `ID3D11Device` in the plugin is independent and safe (no shared
  state). Compression is a brief compute burst; it competes for GPU time but won't corrupt
  PS rendering. Keep the device single and short-lived work-wise (each `Compress` is
  self-contained). If users report UI hitching on weak GPUs, that's the trade-off; WARP or
  CPU fallback remains.
- **Mip chain / cube maps:** the chain `Compress()` overload handles `mipLevels` and
  `arraySize==6` internally (`DirectXTexCompressGPU.cpp:288-339`), matching the metadata
  RitoTex already builds. No manual per-mip loop (unlike the ISPC path at `Codec.cpp:108`).
  Verify cube output specifically (6 faces x mips) since that's the most complex case.
- **sRGB:** GPU BC7 supports `BC7_UNORM_SRGB` (source auto-converted to
  `R8G8B8A8_UNORM_SRGB`, `BCDirectCompute.cpp:217-219`). The plugin currently emits
  `BC7_UNORM` only; if/when sRGB output is added, the GPU path already covers it. The
  `TEX_COMPRESS_SRGB*` flags also flow through (`DirectXTexCompressGPU.cpp:23-29,160`).
- **Quality parity:** GPU BC7 uses the same mode set as CPU by default (3-subset modes off
  unless `TEX_COMPRESS_BC7_USE_3SUBSETS`). Output should be visually equivalent; bit-exact
  match with the CPU encoder is **not** guaranteed (different code path). Acceptable.
- **Image size:** block formats require dims that the encoder pads internally; DirectXTex's
  GPU path computes `xblocks/yblocks` with `(w+3)>>2` (`BCDirectCompute.cpp:198-199`), so
  non-multiple-of-4 is handled. (RitoTex/`.tex` separately enforces /4 upstream — unchanged.)
- **First-call latency:** `GPUCompressBC::Initialize` creates ~7 compute shaders. There's a
  one-time cost per `Compress()` call (the overload makes a fresh `GPUCompressBC`). For a
  single export this is negligible vs the encode savings. (A future optimization could cache
  a `GPUCompressBC`, but that means bypassing the public overload — out of scope.)

### How to verify
1. Build x64 Release; confirm `d3d11.lib` linked and plugin loads in Photoshop.
2. Export a large RGBA image (e.g. 4096x4096) as **BC7 .dds**; time it vs current build —
   expect a large speedup. Round-trip-decode the .dds (DirectXTex `texconv`/`DDSView` or
   the plugin's own reader) and eyeball quality.
3. Export **BC7 with mipmaps** and **BC7 cube map**; verify all mips/faces present and
   correct (open in `DDSView` / a DDS viewer).
4. Confirm **BC1/BC3/BC5** exports are byte-for-byte unchanged from the current build
   (they must not touch the GPU path).
5. Force the fallback: temporarily make `GetCompressDevice()` return nullptr; confirm BC7
   still exports correctly via CPU (regression-proofs the fallback).
6. Test on a no-GPU / RDP session to confirm WARP/CPU fallback doesn't crash or hang.

---

## 6. Implementer checklist (TL;DR)

1. `IntelPlugin.h`: add `#include <d3d11.h>`, `<wrl/client.h>`, `<mutex>` after line 29.
2. `Codec.cpp`: add `CreateCompressDevice()` + cached `GetCompressDevice()` (mutex-guarded);
   in the `useDirectXTex` branch, route BC7 to `Compress(device, ...)` with `alphaWeight=1.0f`,
   fall back to the existing CPU `Compress(...)` on null device or `FAILED(hr)`.
3. `IntelTextureWorks.vcxproj`: add `d3d11.lib` to `AdditionalDependencies` in all 4 `<Link>`
   blocks (lines 126, 175, 228, 285).
4. No DirectXTex project changes; no new source files.
5. Verify per section 5.
