# Photoshop C++ Plug-in SDK — File Format Plugin Audit (for RitoTex `.tex`)

> Reimplementation-grade map of the Adobe Photoshop Plug-in SDK **File Format** module API, grounded in the actual SDK headers and the `SimpleFormat` sample (the gold template).
>
> SDK root: `e:\RitoShark\RitoTex\PhotoshopSDK\pluginsdk`
> Headers: `…\photoshopapi\photoshop` and `…\photoshopapi\pica_sp`
> Gold sample: `…\samplecode\format\simpleformat\` (also `3dformat`, `textformat`)
> Goal: register `.tex` (Riot/LoL BCn texture) so it opens & saves natively via **File ▸ Open** / **Save As**, exactly like a Paint.NET `FileType`.

All file:line references below are to files that were read in full during this audit.

---

## 0. TL;DR mental model

A Format plug-in is a single DLL (renamed to `.8bi` on Windows) exporting one C function, `PluginMain`. Photoshop calls it repeatedly with a **selector** (an `int16`) and a pointer to a **`FormatRecord`** struct. The plug-in:

- **Reading** (File ▸ Open): decodes the file header, tells Photoshop the image dimensions/mode/depth/planes, then hands Photoshop decoded pixels one chunk at a time.
- **Writing** (Save As): receives pixels from Photoshop one chunk at a time and encodes them to the file.

The plug-in declares which file extensions/types it handles, and which color modes it supports, in a **PiPL resource** compiled from a `.r` file (Mac rez syntax) — on Windows the PiPL is embedded via a `.rc` that `#include`s `Cnvtpipl`-style PiPL definitions. Photoshop reads the PiPL to wire `.tex` into the Open/Save dialogs.

This is the direct analogue of Paint.NET's `FileType`: PiPL extensions = `FileType` extensions; ReadStart/ReadContinue = `OnLoad`; WriteStart/WriteContinue = `OnSave`.

---

## 1. File Format plugin model

### 1.1 Module identity

From `PIFormat.h:21-28`:

> Format plug-in modules … are used to add new file types to the **Open**, **Save**, and **Save As** commands. … The file type for format modules for Mac OS is `8BIF`, the extension for Windows is `.8BI`.

So on Windows: **build a DLL, output extension `.8bi`** (Photoshop loads it as a format plug-in). The PiPL `Kind` is `ImageFormat` which maps to the OSType `'8BIF'` (see `PIPL.r:73`: `ImageFormat='8BIF'`).

### 1.2 Entry point signature

From `SimpleFormat.cpp:37-40` and `:183-186` (declaration + definition):

```cpp
DLLExport MACPASCAL void PluginMain (const int16     selector,
                                     FormatRecordPtr formatParamBlock,
                                     intptr_t *      data,
                                     int16 *         result);
```

- `selector` — what to do (the `formatSelector*` constants).
- `formatParamBlock` — pointer to the `FormatRecord` (except for the About selector, where it points to an `AboutRecord` — see below).
- `data` — an `intptr_t*` the plug-in uses to stash a handle/pointer to its own persistent globals between calls (Photoshop preserves it for the life of the operation). SimpleFormat allocates a `Data` handle here (`SimpleFormat.cpp:240-247, 1222-1229`).
- `result` — the plug-in writes its error code here (`noErr` = 0 on success). `PIFormat.h:779-806` lists format error codes.

`DLLExport` / `MACPASCAL` are platform macros (on Win64, `__declspec(dllexport)` + cdecl). The function must be exported by the name referenced in the PiPL `CodeWin64X86 { "PluginMain" }` property (`SimpleFormat.r:94`).

### 1.3 The About selector is special

`SimpleFormat.cpp:210-216`: when `selector == formatSelectorAbout` (= 0), `formatParamBlock` is **not** a `FormatRecord` — it is an `AboutRecordPtr`. Get `sSPBasic` and `plugInRef` from the `AboutRecord`, show the about box, return. Never touch FormatRecord fields in this branch.

### 1.4 Selector constants (full list)

From `PIFormat.h:46-342`. The lifecycle-relevant ones:

| Selector | Value | Phase | Purpose |
|---|---|---|---|
| `formatSelectorAbout` | 0 | — | Show about box. `AboutRecord`, not FormatRecord. (`:59`) |
| `formatSelectorReadPrepare` | 1 | Read | Adjust `maxData` memory budget. (`:67`) |
| `formatSelectorReadStart` | 2 | Read | Parse header; set imageMode/size/depth/planes; alloc first buffer. (`:112`) |
| `formatSelectorReadContinue` | 3 | Read | Decode & deliver pixel chunks; set `data=NULL` when done. (`:126`) |
| `formatSelectorReadFinish` | 4 | Read | Cleanup; free buffers. Always called if ReadStart succeeded. (`:138`) |
| `formatSelectorOptionsPrepare` | 5 | Write | Adjust `maxData`. (`:147`) |
| `formatSelectorOptionsStart` | 6 | Write | Ask user for save options / read script params. (`:158`) |
| `formatSelectorOptionsContinue` | 7 | Write | Optional image scan for options. (`:167`) |
| `formatSelectorOptionsFinish` | 8 | Write | Cleanup. (`:171`) |
| `formatSelectorEstimatePrepare` | 9 | Write | Adjust `maxData`. (`:178`) |
| `formatSelectorEstimateStart` | 10 | Write | Compute disk size → `minDataBytes`/`maxDataBytes`. (`:189`) |
| `formatSelectorEstimateContinue` | 11 | Write | Optional image scan for size. (`:199`) |
| `formatSelectorEstimateFinish` | 12 | Write | Cleanup. (`:203`) |
| `formatSelectorWritePrepare` | 13 | Write | Adjust `maxData`. (`:212`) |
| `formatSelectorWriteStart` | 14 | Write | Write header; request first pixel chunk. (`:255`) |
| `formatSelectorWriteContinue` | 15 | Write | Encode & write chunks. (`:271`) |
| `formatSelectorWriteFinish` | 16 | Write | Write trailer; cleanup. Always called if WriteStart succeeded. (`:285`) |
| `formatSelectorFilterFile` | 17 | Filter | "Can you read this file?" — sniff header. (`:301`) |

Higher selectors (18–41) are advanced/optional: `GetFilePropertyValue`, `LosslessRotate`, `BulkSettings`, `XMPRead/Write`, layer read/write (`ReadLayerStart`=35 … `WriteLayerFinish`=40), `Load`/`Unload`, `Preferences`, settings copy/paste, `LaunchExternalEditor`=41. **For a basic `.tex` flat-image format you only implement selectors 0–17, and several of those are no-ops.**

> **Important note** (`PIFormat.h:784-787`): *When writing a file, if your plug-in sets `result` to any non-zero value, no subsequent selector calls are made.* So if OptionsStart fails, neither estimate nor write runs.

### 1.5 Read lifecycle (File ▸ Open)

Ordered sequence Photoshop drives:

```
formatSelectorAbout            (once, only when user clicks About — independent of open)

[ Open a file ]
  formatSelectorFilterFile     (optional, to confirm the plug-in can read it — sniff magic)
  formatSelectorReadPrepare    -> set maxData (SimpleFormat sets 0)
  formatSelectorReadStart      -> read header; SET imageMode, imageSize32, depth, planes,
                                  imageHRes/VRes, (redLUT/greenLUT/blueLUT if indexed),
                                  transparencyPlane; allocate/announce first pixel buffer
  loop:
    formatSelectorReadContinue -> fill FormatRecord.data with decoded pixels for theRect32 /
                                  loPlane..hiPlane, call advanceState(); when finished set
                                  FormatRecord.data = NULL to end the loop
  formatSelectorReadFinish     -> free buffers, dispose image-resource handle, cleanup
```

SimpleFormat reference: ReadStart `SimpleFormat.cpp:527-712`, ReadContinue `:716-787`, ReadFinish `:791-807`.

Cancellation gotcha (`PIFormat.h:121-124`): if the user cancels mid-ReadContinue, the **next call is ReadFinish, not ReadContinue**. Don't assume another Continue is coming.

### 1.6 Write lifecycle (Save / Save As)

```
[ Save As ]
  formatSelectorOptionsPrepare   -> set maxData
  formatSelectorOptionsStart     -> show save dialog / read script params; if data!=NULL you
                                    may scan image in OptionsContinue
   (formatSelectorOptionsContinue)-> only if data!=NULL in OptionsStart
  formatSelectorOptionsFinish

  formatSelectorEstimatePrepare  -> set maxData
  formatSelectorEstimateStart    -> compute file size; set minDataBytes/maxDataBytes; data=NULL
   (formatSelectorEstimateContinue)
  formatSelectorEstimateFinish

  formatSelectorWritePrepare     -> set maxData
  formatSelectorWriteStart       -> write header; request first chunk: set theRect32,
                                    loPlane/hiPlane, colBytes/rowBytes/planeBytes, data buffer;
                                    call advanceState() to pull pixels from Photoshop, encode,
                                    write to file
  loop:
    formatSelectorWriteContinue  -> (SimpleFormat does the whole image inside WriteStart's
                                    own loop using advanceState, so its WriteContinue is empty)
  formatSelectorWriteFinish      -> write trailer / script params
```

SimpleFormat reference: OptionsStart `:818-821` (sets `data=NULL` → no OptionsContinue), EstimateStart `:844-863`, WriteStart `:886-1045` (does the full per-plane/per-row write loop internally), WriteContinue `:1049-1051` (empty), WriteFinish `:1055-1058`.

**Two valid styles** (both shown by SimpleFormat):
1. **Self-driving** (SimpleFormat): do the whole image inside `WriteStart`/`ReadContinue` by looping and calling `advanceState()` after setting each chunk descriptor. `WriteContinue` stays empty.
2. **Host-driving**: set up one chunk in Start, return; Photoshop calls Continue again for the next chunk until you signal completion (`data=NULL` for read, empty `theRect` for write).

The self-driving style is simplest for `.tex` and is what to copy.

### 1.7 FilterFile (sniffing)

`formatSelectorFilterFile` (`SimpleFormat.cpp:1062-1088`): seek to start, read header, check magic. Return `noErr` if "I can read this", or `formatCannotRead` (`PIFormat.h:796`) otherwise. This is how Photoshop disambiguates when a `.tex` extension could be served by multiple plug-ins, and how it identifies files with the wrong extension.

---

## 2. FormatRecord fields (the ones that matter)

`FormatRecord` is defined in `PIFormat.h:819-1396`. It is `#pragma pack(push,4)` aligned on Win32 (`:808-810`). Quote-level field references:

### 2.1 Image geometry & format (you SET these in ReadStart; Photoshop SETS them before write)

```c
int16   imageMode;     // PIFormat.h:878  grayscale/RGB/indexed/etc. See §5.
Point   imageSize;     // :886  DEPRECATED — use imageSize32.
int16   depth;         // :888  bits per pixel per plane: 1, 8, 16, or 32.
int16   planes;        // :892  channel count (RGB=3, RGBA=4). Never exceed 16 planes at once.
Fixed   imageHRes;     // :904  horizontal DPI, 16.16 fixed. Default 72<<16.
Fixed   imageVRes;     // :912  vertical DPI (PS ignores but set it anyway).
VPoint  imageSize32;   // :1242 32-bit {v=height, h=width}. THE field to use (see §2.7).
```

`VPoint` is `{int32 v; int32 h;}` (v = vertical/rows/height, h = horizontal/cols/width). SimpleFormat sets it via `SetFormatImageSize` (`:1182-1195`).

### 2.2 Color tables (indexed mode only)

```c
LookUpTable redLUT;    // :925
LookUpTable greenLUT;  // :932
LookUpTable blueLUT;   // :939
int32       lutCount;  // :1159 number of entries in indexed table
int32       transparentIndex; // :1134 GIF-style transparent index if <256
```
`.tex` is BCn/RGBA so these are irrelevant — leave them.

### 2.3 The pixel-transfer descriptor (the heart of read/write)

These describe the layout of the buffer you hand to / receive from Photoshop. You set them in the Start/Continue handlers:

```c
void * data;        // :947  pointer to the pixel buffer. Set to NULL to END a read loop.
Rect   theRect;     // :954  DEPRECATED — use theRect32.
int16  loPlane;     // :956  first channel index covered by 'data'
int16  hiPlane;     // :961  last channel index covered by 'data'
int16  colBytes;    // :966  byte stride between columns (pixels) within a row
int32  rowBytes;    // :970  byte stride between rows
int32  planeBytes;  // :972  byte stride between planes (ignored if loPlane==hiPlane; 1 for interleaved)
PlaneMap planeMap;  // :977  channel remap; host inits to identity planeMap[i]=i
VRect  theRect32;   // :1252 32-bit region this buffer covers {top,left,bottom,right}
int32  maxValue;    // :1129 for 16-bit reads: the white value (SimpleFormat uses 0x8000)
```

**Two canonical layouts** (documented in `PIFormat.h:88-110` for read, `:227-253` for write):

- **Planar / one channel at a time** (what SimpleFormat does):
  `loPlane = hiPlane = N; colBytes = bytesPerSample; rowBytes = width*bytesPerSample; planeBytes = 0` and loop N over each plane. (`SimpleFormat.cpp:750-779`).

- **Interleaved RGB(A)** (best for `.tex` after BCn decode → RGBA8):
  ```
  loPlane = 0;
  hiPlane = 3;                       // RGBA = 4 planes (0..3)
  colBytes = 4;                      // 4 bytes per pixel (interleaved)
  rowBytes = width * 4;
  planeBytes = 1;                    // 1 byte between successive planes of a pixel
  ```
  Then a single `advanceState()` per row (or per whole image if you allocate the whole frame).

**How pixels move:**
- **Read**: you fill `data` with decoded pixels matching the descriptor, set `theRect32`/`loPlane`/`hiPlane`, then call `gFormatRecord->advanceState()` (`PIFormat.h:1093`). That hands the chunk to Photoshop. (`SimpleFormat.cpp:770-775`).
- **Write**: you set the descriptor and call `advanceState()` *first* — Photoshop *fills* your `data` buffer with the requested region — then you encode/write it. (`SimpleFormat.cpp:1025-1031`).

### 2.4 File I/O — how you actually touch file bytes

```c
intptr_t dataFork;  // :858  the OS file handle (on Windows, the HANDLE/fd from OpenFile()).
intptr_t rsrcFork;  // :863  resource fork (undefined on Windows).
FSSpec * fileSpec;  // :871  full file spec (nil on Mac64).
SPPlatformFileSpecificationW *fileSpec2; // :1308 Unicode/cross-platform replacement.
```

You do **not** open the file yourself — Photoshop opens it and gives you `dataFork`. You read/write through SDK file helpers that operate on `dataFork`. SimpleFormat wraps them (`SimpleFormat.cpp:452-505`):

```cpp
*gResult = PSSDKRead  (gFormatRecord->dataFork, &count, buffer);  // :460
*gResult = PSSDKWrite (gFormatRecord->dataFork, &count, buffer);  // :477
*gResult = PSSDKSetFPos(gFormatRecord->dataFork, fsFromStart, 0); // :549  (seek)
```

`PSSDKRead/PSSDKWrite/PSSDKSetFPos` and `fsFromStart` come from the SDK's `FileUtilities` library (`SimpleFormat.h:33` includes `"FileUtilities.h"`). They are thin cross-platform wrappers over the `dataFork` handle. `count` is in/out: after read it holds bytes actually read; SimpleFormat treats short read as `eofErr` (`:462-463`) and short write as `dskFulErr` (`:479-480`).

> For `.tex`: in ReadStart, `PSSDKSetFPos(dataFork, fsFromStart, 0)` then `PSSDKRead` the whole `.tex` payload into a buffer, hand it to your external `DecodeTex(...)`. In WriteStart, encode RGBA → `.tex` bytes via `EncodeTex(...)` then `PSSDKWrite`.

### 2.5 Callbacks / suites carried in the record

```c
TestAbortProc   abortProc;     // :829  user-cancel test
ProgressProc    progressProc;  // :831  progress bar: progressProc(done,total)  (used :775,:1033)
AdvanceStateProc advanceState; // :1093 the chunk pump (REQUIRED; checked non-NULL at :225)
BufferProcs *   bufferProcs;   // :1067 old buffer suite
HandleProcs *   handleProcs;   // :1082 old handle suite
ResourceProcs * resourceProcs; // :1072 pseudo-resource suite (image resources)
PropertyProcs * propertyProcs; // :1104 property suite
SPBasicSuite *  sSPBasic;      // :1130 PICA basic suite — gateway to ALL PICA suites
void *          plugInRef;     // :1132 PICA plug-in ref
```

`sSPBasic` is how you `AcquireSuite` the modern PICA Handle/Buffer suites (see §4). SimpleFormat grabs it at `:220`.

### 2.6 Memory / size budgeting

```c
int32 maxData;       // :833  max bytes PS will give you; reduce in *Prepare handlers
int32 minDataBytes;  // :841  set in EstimateStart = min file size
int32 maxDataBytes;  // :845  set in EstimateStart = max file size
int32 minRsrcBytes / maxRsrcBytes; // :850/:853 resource-fork sizes (0 on Windows for .tex)
```

SimpleFormat sets `maxData = 0` in every *Prepare* (e.g. `:447`) — fine for a plug-in that manages its own buffers.

### 2.7 32-bit coordinate flag (MANDATORY for modern PS)

```c
int32 HostSupports32BitCoordinates;  // :1236
int32 PluginUsing32BitCoordinates;   // :1239
```

`SimpleFormat.cpp:232-233`:
```cpp
if (gFormatRecord->HostSupports32BitCoordinates)
    gFormatRecord->PluginUsing32BitCoordinates = true;
```
**You must opt in.** Once opted in, use `imageSize32`/`theRect32` (32-bit), NOT the legacy 16-bit `imageSize`/`theRect`. SimpleFormat's `GetFormatImageSize`/`SetFormatTheRect` helpers branch on these flags (`:1166-1213`). For `.tex` (textures can be ≥ 4096 but rarely > 32767) opt in anyway — it's the supported path.

### 2.8 Transparency / alpha

```c
int32 transparencyPlane;   // :1189  read: PS sets -1 if it supports transparency; you set it to
                           //         the index of the alpha plane. write: index of alpha plane.
int32 transparencyMatting; // :1204  0=no matte (unassociated/straight alpha), 1/2/3=black/gray/white
```
SimpleFormat sets `transparencyMatting = DESIREDMATTING` (=0, straight alpha) (`:618, :1012`). For `.tex` with alpha, set `transparencyPlane` to your alpha plane index and `transparencyMatting = 0` (straight/unassociated alpha, matching BCn).

### 2.9 ICC profile (optional)

```c
Handle iCCprofileData; int32 iCCprofileSize; int32 canUseICCProfiles; // :1137-1154
```
SimpleFormat reads/writes the profile inline (`:1123-1164`). `.tex` has no profile — skip (leave `canUseICCProfiles` path unused).

### 2.10 Misc useful

```c
Boolean openForPreview; // :1219 true when PS just wants a thumbnail — do minimal work
Boolean openAsSmartObject; // :1350 place result as smart object
Str255 * errorString;   // :1122 set + return result=errReportString to show a custom error
```

---

## 3. PiPL resource — declaring the format to Photoshop

The PiPL ("Plug-In Property List") is the manifest Photoshop reads to know the plug-in's kind, entry point, supported modes, and — critically — which file **extensions/types** it reads & writes. Format-specific PiPL property keys are defined in `PIFormat.h:344-553`; the rez templates are in `PIPL.r:628-890`.

### 3.1 Key format PiPL properties (from `PIFormat.h` + `PIPL.r`)

| PiPL `.r` name | key | meaning | `PIFormat.h` | `PIPL.r` |
|---|---|---|---|---|
| `Kind { ImageFormat }` | `'kind'`=`'8BIF'` | it's a format module | — | `:73,:119` |
| `FmtFileType` | `'fmTC'` | default Mac type/creator pair | `:372` | `:631-637` |
| `ReadTypes` | `'RdTy'` | Mac type/creator pairs it can read | `:380` | `:641-646` |
| `WriteTypes` | `'WrTy'` | type/creator pairs it writes | — | `:648-653` |
| `FilteredTypes` | `'fftT'` | types to ask FilterFile about | `:390` | `:658-663` |
| `ReadExtensions` | `'RdEx'` | **Windows file extensions it reads** | `:398` | `:670-675` |
| `WriteExtensions` | `'WrEx'` | **extensions it writes** | — | `:677-682` |
| `FilteredExtensions` | `'fftE'` | extensions to ask FilterFile about | `:407` | `:687-692` |
| `FormatFlags` | `'fmtf'` | can-read/can-write/transparency flags | `:415,:594-654` | `:694-709` |
| `FormatMaxSize` | `'mxsz'` | max rows/cols (16-bit era) | `:445` | `:711-716` |
| `PlugInMaxSize` | `'ms32'` | max rows/cols (32-bit) | — | `:510-516` |
| `FormatMaxChannels` | `'mxch'` | max channels per mode (12-entry array) | `:458` | `:718-724` |
| `FormatICCFlags` | `'fmip'` | which modes can embed ICC | `:429` | `:738-750` |
| `SupportedModes` | `'mode'` | color modes the plug-in supports | — | `:436-458` |
| `EnableInfo` | `'enbl'` | enable expression (`"true"` = always) | — | `:460-472` |

**Extension encoding (critical):** extensions are stored as an `OSType` (4 chars), where *the extension goes in the first 3 chars and the 4th MUST be a space* (`PIFormat.h:396-398`). So `.tex` → `'tex '` (t, e, x, space). `.SME` in SimpleFormat is `'SME '` (`SimpleFormat.r:132-134`).

**FormatFlags values** (rez booleans, `PIPL.r:699-707`, indices `PIFormat.h:605-652`):
`fmtSavesImageResources` (bit1), `fmtCanRead` (bit2), `fmtCanWrite` (bit3), `fmtCanWriteIfRead` (bit4), `fmtCanWriteTransparency` (bit5), `fmtCanCreateThumbnail` (bit6).

### 3.2 The 12-mode ordering (for SupportedModes & FormatMaxChannels)

Both `SupportedModes` (`PIPL.r:446-455`) and `FormatMaxChannels` (`SimpleFormat.r:126-127`) use this fixed order (= the `plugInMode*` enum, see §5):

```
0 Bitmap, 1 GrayScale, 2 IndexedColor, 3 RGBColor, 4 CMYKColor,
5 HSLColor, 6 HSBColor, 7 Multichannel, 8 Duotone, 9 LABColor   (+2 reserved → 12 slots)
```
SimpleFormat's `FormatMaxChannels { { 1, 24, 24, 24, … } }` means: Bitmap=1 channel, everything else up to 24. `maxChannels[plugInModeRGBColor]` (index 3) is the RGB(+alpha) cap.

### 3.3 Concrete `.tex` PiPL (Mac `.r` form) — copy & adapt

```c
// TexFormat.r  (Mac rez form; Windows uses the .rc include, see §6)
#define plugInName          "RitoTex"
#define plugInCopyrightYear "2026"
#define plugInDescription   "Riot/League of Legends .tex (BCn) texture format."

#define vendorName          "RitoTex"
#define plugInSuiteID       'rTx1'
#define plugInClassID       'rTeX'
#define plugInEventID       typeNull   // must be typeNull for format plug-ins

#include "PIDefines.h"
#if __PIMac__
    #include "Types.r"
    #include "SysTypes.r"
    #include "PIGeneral.r"
    #include "PIUtilities.r"
#elif defined(__PIWin__)
    #define Rez
    #include "PIGeneral.h"
    #include "PIUtilities.r"
#endif
#include "PITerminology.h"
#include "PIActions.h"

resource 'PiPL' (ResourceID, plugInName " PiPL", purgeable)
{
    {
        Kind { ImageFormat },
        Name { plugInName },
        Version { (latestFormatVersion << 16) | latestFormatSubVersion },

        #if defined(_WIN64)
            CodeWin64X86 { "PluginMain" },
        #else
            CodeWin32X86 { "PluginMain" },
        #endif

        // ClassID, eventID(typeNull), aete ID, unique scope string
        HasTerminology { plugInClassID, plugInEventID, ResourceID,
                         vendorName " " plugInName },

        // .tex is RGB / RGBA (BCn decodes to 8-bit RGBA). Advertise RGB only to start.
        SupportedModes
        {
            noBitmap, noGrayScale,
            noIndexedColor, doesSupportRGBColor,
            noCMYKColor, noHSLColor,
            noHSBColor, noMultichannel,
            noDuotone, noLABColor
        },

        EnableInfo { "in (PSHOP_ImageMode, RGBMode)" },   // or just "true"

        PlugInMaxSize  { 16384, 16384 },        // 32-bit cap (textures)
        FormatMaxSize  { { 16384, 16384 } },    // 16-bit-era cap

        // 12-slot channel caps: index 3 (RGB) allows 4 (RGBA).
        FormatMaxChannels { { 0, 0, 0, 4, 0, 0, 0, 0, 0, 4, 0, 0 } },

        // Mac type/creator (ignored on Windows but harmless):
        FmtFileType { 'tex ', '8BIM' },

        // *** THE EXTENSION REGISTRATION ***  ('tex' + trailing space)
        ReadExtensions     { { 'tex ' } },
        WriteExtensions    { { 'tex ' } },
        FilteredExtensions { { 'tex ' } },

        FormatFlags {
            fmtSavesImageResources,   // leave even if you don't use resources
            fmtCanRead,
            fmtCanWrite,
            fmtCanWriteIfRead,
            fmtCanWriteTransparency
        },

        FormatICCFlags { iccCannotEmbedGray, iccCannotEmbedIndexed,
                         iccCannotEmbedRGB, iccCannotEmbedCMYK }
    }
};
```

A minimal `aete` (scripting dictionary) and `StringResource` history block can be copied verbatim from `SimpleFormat.r:153-202`; they are required by `HasTerminology` but otherwise boilerplate.

> The `ResourceID`, `latestFormatVersion`, `latestFormatSubVersion`, mode booleans (`doesSupportRGBColor`, `noBitmap`, …) and flag tokens (`fmtCanRead`, etc.) all come from the SDK's `PIGeneral.r` / `PIUtilities.r` / `PIPL.r`, which `SimpleFormat.r` includes (`:56-72`).

---

## 4. PICA suites — acquiring Handle & Buffer suites

A Format plug-in allocates its **pixel buffer** with the **Buffer suite** and its **image-resource / data handles** with the **Handle suite**. Both are reached through `sSPBasic` (the PICA basic suite carried in `FormatRecord.sSPBasic`, `PIFormat.h:1130`).

### 4.1 Acquire pattern (from the suite headers)

Handle suite (`PIHandleSuite.h:47-55, :78, :98-100, :122-132`):
```cpp
#include "PIHandleSuite.h"
PSHandleSuite2 *sPSHandle = NULL;
SPErr err = sSPBasic->AcquireSuite(
    kPSHandleSuite,            // "Photoshop Handle Suite for Plug-ins"
    kPSHandleSuiteVersion2,    // 2
    (const void**)&sPSHandle);
// use it…
sSPBasic->ReleaseSuite(kPSHandleSuite, kPSHandleSuiteVersion2);
```
`PSHandleSuite2` members (`:122-132`): `New(size)`, `Dispose(h)`, `DisposeRegularHandle(h)`, `SetLock(h, lock, &ptr, &oldLock)`, `GetSize(h)`, `SetSize(h,size)`, `RecoverSpace(size)`.

Buffer suite (`PIBufferSuite.h:42-49, :70, :156-186`):
```cpp
#include "PIBufferSuite.h"
PSBufferSuite2 *sPSBuffer = NULL;
SPErr err = sSPBasic->AcquireSuite(
    kPSBufferSuite,            // "Photoshop Buffer Suite for Plug-ins"
    kPSBufferSuiteVersion2,    // 2 (v1 also exists)
    (const void**)&sPSBuffer);
…
sSPBasic->ReleaseSuite(kPSBufferSuite, kPSBufferSuiteVersion2);
```
`PSBufferSuite1` members (`:159-166`): `New(&requestedSize, minSize)→Ptr`, `Dispose(&ptr)`, `GetSize(ptr)`, `GetSpace()`. `PSBufferSuite2` adds 64-bit variants (`New64`, `GetSize64`, `GetSpace64`).

> The SDK utility library (`PIUtilities`, included by SimpleFormat) wraps this so the sample code can just write `sPSHandle->New(...)` and `sPSBuffer->New(...)` (e.g. `SimpleFormat.cpp:625, :737`) and calls `PIUSuitesRelease()` on finish (`:337`). You can either use that utility layer or call `sSPBasic->AcquireSuite/ReleaseSuite` directly.

### 4.2 Buffer suite usage (pixel buffer)

`SimpleFormat.cpp:736-783`:
```cpp
unsigned32 bufferSize = RowBytes();
Ptr pixelData = sPSBuffer->New(&bufferSize, bufferSize); // alloc
…                                                         // fill / advanceState()
sPSBuffer->Dispose(&pixelData);                           // free (NULLs the ptr)
```

### 4.3 Handle suite usage (image-resource & ICC handles)

`SimpleFormat.cpp:625-674`: `sPSHandle->New(size)`, lock with `SetLock(h,true,&ptr,&old)`, use `ptr`, unlock `SetLock(h,false,…)`, `Dispose(h)`.

> For a flat `.tex`, you mainly need the **Buffer suite** (pixel scanlines/frame) plus the **Handle suite** only if you allocate the per-document `Data` globals handle (SimpleFormat does, `:1222-1229`, but you could use a static/global struct instead).

---

## 5. Color modes / bit depth

### 5.1 Image-mode constants (`plugInMode*`)

`FormatRecord.imageMode` (`PIFormat.h:878`) uses the `plugInMode*` enum, defined verbatim in **`PIGeneral.h:870-887`** (the same 0–9 ordering is mirrored by `SupportedModes` in `PIPL.r:446-455` and by SimpleFormat's `FormatMaxChannels`). Exact values from the header:

| Mode | Value | `PIGeneral.h` | Notes |
|---|---|---|---|
| `plugInModeBitmap` | 0 | `:870` | 1-bit |
| `plugInModeGrayScale` | 1 | `:871` | 8-bit gray |
| `plugInModeIndexedColor` | 2 | `:872` | uses redLUT/greenLUT/blueLUT |
| `plugInModeRGBColor` | 3 | `:873` | **8-bit RGB — the target for `.tex`** |
| `plugInModeCMYKColor` | 4 | `:874` | |
| `plugInModeHSLColor` | 5 | `:875` | |
| `plugInModeHSBColor` | 6 | `:876` | |
| `plugInModeMultichannel` | 7 | `:877` | |
| `plugInModeDuotone` | 8 | `:878` | |
| `plugInModeLabColor` | 9 | `:879` | |
| `plugInModeGray16` | 10 | `:880` | 16-bit gray |
| `plugInModeRGB48` | 11 | `:881` | 16-bit RGB |
| `plugInModeLab48` | 12 | `:882` | 16-bit Lab |
| `plugInModeCMYK64` | 13 | `:883` | 16-bit CMYK |
| `plugInModeDeepMultichannel` | 14 | `:884` | |
| `plugInModeDuotone16` | 15 | `:885` | |
| `plugInModeRGB96` | 16 | `:886` | 32-bit float RGB |
| `plugInModeGray32` | 17 | `:887` | 32-bit float gray |

The 8-bit RGB case you need is `plugInModeRGBColor` = 3 (confirmed `PIGeneral.h:873`).

### 5.2 Depth & how `.tex` maps

`FormatRecord.depth` (`PIFormat.h:888`) = bits **per sample per plane**: valid 1, 8, 16, 32.

- BCn (`.tex` BC1/BC3/BC7) decodes to **8-bit RGBA** → `imageMode = plugInModeRGBColor`, `depth = 8`, `planes = 4` (RGB + alpha), or `planes = 3` if opaque.
- If you ever decode to 16-bit, use `plugInModeRGB48` + `depth = 16` and set `maxValue = 0x8000` on read (SimpleFormat `:754-755`).

### 5.3 Alpha / transparency

- Set `planes = 4` and tell Photoshop the 4th plane is alpha by setting `transparencyPlane` to the alpha index (read: `PIFormat.h:1199-1202`).
- `transparencyMatting = 0` for straight/unassociated alpha (matches BCn's non-premultiplied alpha). (`PIFormat.h:1204-1210`, SimpleFormat `:1012`.)
- Cap channels per mode in PiPL `FormatMaxChannels` (index 3 = RGB → 4).

### 5.4 Planar vs interleaved layout

Photoshop accepts **either**, controlled purely by the `colBytes/rowBytes/planeBytes/loPlane/hiPlane` descriptor (§2.3):

- **Planar** (SimpleFormat): one channel per `advanceState`, `loPlane==hiPlane`, `colBytes=bytesPerSample`, `planeBytes=0`.
- **Interleaved RGBA8** (recommended for `.tex`): `loPlane=0, hiPlane=3, colBytes=4, rowBytes=width*4, planeBytes=1`, deliver the whole interleaved frame (or row) in one `advanceState`.

Internally Photoshop stores RGBA in **R,G,B,A** plane order; if your decoder produces a different order use `planeMap` (`PIFormat.h:977-986`) to remap rather than shuffling bytes.

---

## 6. Build (Windows, x64)

All facts below are verbatim from the real `samplecode\format\simpleformat\win\SimpleFormat.vcxproj` and `SimpleFormat.rc`.

### 6.1 Project shape (proven from `SimpleFormat.vcxproj`)

- **Configuration type:** `DynamicLibrary` (`.vcxproj:19,23`).
- **Platform:** the sample ships `Debug|Win32` and `Debug|x64` (`:4-11`). Build **x64** for modern Photoshop; PiPL must use `CodeWin64X86 { "PluginMain" }`.
- **Output extension `.8bi`:** set per-config via `<TargetExt>.8bi</TargetExt>` (`:52-53`) and the linker `<OutputFile>…\SimpleFormat.8bi</OutputFile>` (`:89,:136`). Photoshop only loads format plug-ins with the `.8bi` extension. The x64 link uses `<TargetMachine>MachineX64</TargetMachine>` (`:146`).
- **Exports:** `PluginMain` is exported via the `DLLExport` macro on the function (`SimpleFormat.cpp:183`) — matches the PiPL `CodeWin64X86 { "PluginMain" }` code property. (`PIDLLInstance.cpp` is compiled in too, `:201`, providing `DllMain`.)
- **Runtime/defines:** `RuntimeLibrary=MultiThreadedDebug`, defines `ISOLATION_AWARE_ENABLED=1;WIN32=1;_WINDOWS;_CRT_SECURE_NO_DEPRECATE;_SCL_SECURE_NO_DEPRECATE` (`:69,:116`). Extra options `/MP /GS` (`:66,:113`). Link deps `odbc32.lib;odbccp32.lib;version.lib` (`:88,:135`).
- **Support sources compiled into the DLL** (`:150-232`): `SimpleFormat.cpp`, `SimpleFormatScripting.cpp`, `SimpleFormatUI.cpp`, plus the shared SDK utility layer `FileUtilities.cpp`, `FileUtilitiesWin.cpp`, `PIDLLInstance.cpp`, `PIUSuites.cpp`, `PIUtilities.cpp`, `PIUtilitiesWin.cpp`, `PIWinUI.cpp`, `DialogUtilitiesWin.cpp`. (For `.tex` you can drop the UI/scripting ones and keep `FileUtilities*` for `PSSDKRead/Write/SetFPos`.)

### 6.2 Include paths (verbatim from `:68,:115`)

C/C++ → Additional Include Directories (relative to `win\`):
```
..\Common
..\..\..\Common\Includes        (PIUtilities.h, FileUtilities.h, PIDefines.h)
..\..\..\..\PhotoshopAPI\Photoshop   (PIFormat.h, PIGeneral.h, PITypes.h, suites…)
..\..\..\..\PhotoshopAPI\PICA_SP     (SPBasic.h, SPTypes.h…)
```
i.e. absolute equivalents:
```
…\pluginsdk\samplecode\common\includes
…\pluginsdk\photoshopapi\photoshop
…\pluginsdk\photoshopapi\pica_sp
…\pluginsdk\photoshopapi\resources   (for the .r/PiPL compile, see 6.3)
```

### 6.3 Resource (.r → PiPL → .rc) compilation — the exact SDK pipeline

The sample uses a **two-stage** pipeline (proven from `SimpleFormat.vcxproj:238-260` + `SimpleFormat.rc:93`):

1. **CustomBuild step on `SimpleFormat.r`** (`:239-252`): preprocess the `.r` with `cl /EP /DMSWindows=1` (include dirs `PICA_SP`, `Photoshop`, `Resources`, `Common\Resources`, `Common\Includes`) to a temp `.rr`, then run **`cnvtpipl`** (ships at `…\samplecode\Resources\cnvtpipl`) to convert it to a binary **`.pipl`** file in `$(IntDir)`:
   ```
   cl /I…\PICA_SP /I…\Photoshop /I…\Resources /I…\Common\Resources /I…\Common\Includes \
      /EP /DMSWindows=1 /Tc"SimpleFormat.r" > "$(IntDir)SimpleFormat.rr"
   ..\..\..\Resources\cnvtpipl "$(IntDir)SimpleFormat.rr" "$(IntDir)SimpleFormat.pipl"
   del "$(IntDir)SimpleFormat.rr"
   ```
2. **The `.rc` `#include`s that generated `.pipl`** (`SimpleFormat.rc:49,93`): `#include "SimpleFormat.pipl"` (and `#include "About.rc"`). The `ResourceCompile` item adds `$(IntDir)` to its include path so `rc.exe` finds the generated `.pipl` (`vcxproj:257,259`). The result is a binary `'PiPL'` resource linked into the DLL.

Practically for `.tex`: copy `win\SimpleFormat.vcxproj` + `SimpleFormat.rc` + `simpleformat-sym.h`, rename to TexFormat, point the CustomBuild at your `TexFormat.r`, keep the `cnvtpipl` step, and have `TexFormat.rc` do `#include "TexFormat.pipl"`. (`MSWindows` is defined for the `.r` preprocess so the PiPL `.r` takes its Windows code path, `SimpleFormat.r:63-67`; `PIPL.r:57-59` skips the Mac-only rez template when `MSWindows` is set.)

### 6.4 Where Photoshop loads plug-ins from

- The Photoshop install `…\Plug-ins\` folder (and `…\Plug-ins\File Formats\`), **or**
- An *Additional Plug-ins Folder* set in **Preferences ▸ Plug-Ins**.

Drop `RitoTex.8bi` there and restart Photoshop. `.tex` then appears in File ▸ Open (and "All Formats") and in the Save As ▸ Format dropdown.

> The two repos `E:\RitoShark\RitoTex-Photoshop` and `E:\RitoShark\Photoshop-Plugin` already contain `TEX_SAVE_IMPLEMENTATION.md` and a `TEX_UseDDSHandler.reg` — worth diffing against this map; they likely hold the actual BCn encode/decode the skeleton below should call into.

---

## 7. Concrete annotated `.tex` Format plugin skeleton

Minimal, self-driving (SimpleFormat style), interleaved RGBA8, wired to external `DecodeTex`/`EncodeTex`. Flesh out the two `extern` functions with the BCn logic from the Paint.NET plugin.

```cpp
//==============================================================================
// TexFormat.cpp  —  Photoshop File Format plug-in for Riot/LoL .tex (BCn)
//==============================================================================
#include "PIDefines.h"
#include "PIFormat.h"
#include "PIHandleSuite.h"
#include "PIBufferSuite.h"
#include "FileUtilities.h"   // PSSDKRead / PSSDKWrite / PSSDKSetFPos / fsFromStart
#include <cstring>

//--- External codec (provide these; port from the Paint.NET FileType) ----------
// Decode a whole .tex file (already read into 'fileBytes') to interleaved RGBA8.
// Returns true on success and fills width/height + a malloc'd RGBA8 buffer
// (row-major, 4 bytes/pixel, R,G,B,A). Caller frees with free().
extern bool DecodeTex(const void* fileBytes, size_t fileSize,
                      int* outW, int* outH, unsigned char** outRGBA);
// Encode interleaved RGBA8 (w*h*4) to .tex bytes (malloc'd). Caller frees.
extern bool EncodeTex(const unsigned char* rgba, int w, int h,
                      void** outBytes, size_t* outSize);

//--- Globals (single-threaded per operation; mirror SimpleFormat) --------------
SPBasicSuite*    sSPBasic   = NULL;
FormatRecordPtr  gFmt       = NULL;
int16*           gResult    = NULL;
intptr_t*        gDataH     = NULL;       // host-preserved scratch (unused here)
PSHandleSuite2*  sPSHandle  = NULL;
PSBufferSuite2*  sPSBuffer  = NULL;

static const char  kTexMagic[4] = { 'T','E','X','\0' };  // adjust to real .tex magic

//--- Suite helpers -------------------------------------------------------------
static void AcquireSuites() {
    sSPBasic->AcquireSuite(kPSHandleSuite, kPSHandleSuiteVersion2, (const void**)&sPSHandle);
    sSPBasic->AcquireSuite(kPSBufferSuite, kPSBufferSuiteVersion2, (const void**)&sPSBuffer);
}
static void ReleaseSuites() {
    if (sPSHandle) sSPBasic->ReleaseSuite(kPSHandleSuite, kPSHandleSuiteVersion2);
    if (sPSBuffer) sSPBasic->ReleaseSuite(kPSBufferSuite, kPSBufferSuiteVersion2);
    sPSHandle = NULL; sPSBuffer = NULL;
}

//--- File I/O wrappers (operate on FormatRecord.dataFork) -----------------------
static void ReadSome (int32 n, void* buf) {
    int32 c = n;
    if (*gResult != noErr) return;
    *gResult = PSSDKRead(gFmt->dataFork, &c, buf);
    if (*gResult == noErr && c != n) *gResult = eofErr;
}
static void WriteSome(int32 n, void* buf) {
    int32 c = n;
    if (*gResult != noErr) return;
    *gResult = PSSDKWrite(gFmt->dataFork, &c, buf);
    if (*gResult == noErr && c != n) *gResult = dskFulErr;
}
static void SeekStart() { *gResult = PSSDKSetFPos(gFmt->dataFork, fsFromStart, 0); }

//--- 32-bit coordinate helpers --------------------------------------------------
static void SetSize32(int32 w, int32 h) { gFmt->imageSize32.h = w; gFmt->imageSize32.v = h; }
static void SetRect32(int32 t,int32 l,int32 b,int32 r){
    gFmt->theRect32.top=t; gFmt->theRect32.left=l; gFmt->theRect32.bottom=b; gFmt->theRect32.right=r;
}

//================================ READ ========================================//
static void DoReadPrepare() { gFmt->maxData = 0; }

static unsigned char* gDecoded = NULL;   // RGBA8 frame held between ReadStart→ReadFinish
static int            gW = 0, gH = 0;

static void DoReadStart() {
    // 1) slurp the whole .tex into memory
    SeekStart(); if (*gResult != noErr) return;
    int32 fileSize = (int32) PSSDKGetFPosEnd(gFmt->dataFork); // or stat via fileSpec2
    SeekStart();
    Ptr raw = sPSBuffer->New((unsigned32*)&fileSize, fileSize);
    if (!raw) { *gResult = memFullErr; return; }
    ReadSome(fileSize, raw);
    if (*gResult != noErr) { sPSBuffer->Dispose(&raw); return; }

    // 2) decode to RGBA8 via external codec
    if (!DecodeTex(raw, (size_t)fileSize, &gW, &gH, &gDecoded)) {
        sPSBuffer->Dispose(&raw); *gResult = formatCannotRead; return;
    }
    sPSBuffer->Dispose(&raw);

    // 3) describe the image to Photoshop
    gFmt->imageMode = plugInModeRGBColor;   // 3
    gFmt->depth     = 8;
    gFmt->planes    = 4;                     // RGBA
    gFmt->imageHRes = gFmt->imageVRes = 72L << 16;
    SetSize32(gW, gH);
    gFmt->transparencyPlane   = 3;          // 4th plane = alpha (host set it to -1 if supported)
    gFmt->transparencyMatting = 0;          // straight/unassociated alpha
}

static void DoReadContinue() {
    if (!gDecoded) { gFmt->data = NULL; return; }

    // Deliver the whole frame interleaved RGBA8 in one shot.
    gFmt->loPlane   = 0;
    gFmt->hiPlane   = 3;
    gFmt->colBytes  = 4;
    gFmt->rowBytes  = gW * 4;
    gFmt->planeBytes= 1;
    gFmt->data      = gDecoded;
    SetRect32(0, 0, gH, gW);

    if (*gResult == noErr) *gResult = gFmt->advanceState();   // push to PS
    if (gFmt->progressProc) gFmt->progressProc(gH, gH);

    gFmt->data = NULL;       // signal end of read loop
}

static void DoReadFinish() {
    if (gDecoded) { free(gDecoded); gDecoded = NULL; }
    gW = gH = 0;
}

//================================ WRITE =======================================//
static void DoOptionsPrepare()  { gFmt->maxData = 0; }
static void DoOptionsStart()    { gFmt->data = NULL; }   // no per-pixel options scan
static void DoOptionsContinue() {}
static void DoOptionsFinish()   {}

static void DoEstimatePrepare() { gFmt->maxData = 0; }
static void DoEstimateStart() {
    // rough upper bound; PS just needs disk-space estimate
    int32 w = gFmt->imageSize32.h, h = gFmt->imageSize32.v;
    int32 bytes = 128 /*header*/ + w * h * 4;   // conservative
    gFmt->minDataBytes = gFmt->maxDataBytes = bytes;
    gFmt->data = NULL;
}
static void DoEstimateContinue() {}
static void DoEstimateFinish()   {}

static void DoWritePrepare() { gFmt->maxData = 0; }

static void DoWriteStart() {
    int32 w = gFmt->imageSize32.h, h = gFmt->imageSize32.v;

    // 1) pull the whole interleaved RGBA frame from Photoshop
    unsigned32 frameSize = (unsigned32)(w * h * 4);
    Ptr frame = sPSBuffer->New(&frameSize, frameSize);
    if (!frame) { *gResult = memFullErr; return; }

    gFmt->loPlane   = 0;
    gFmt->hiPlane   = 3;
    gFmt->colBytes  = 4;
    gFmt->rowBytes  = w * 4;
    gFmt->planeBytes= 1;
    gFmt->data      = frame;
    gFmt->transparencyMatting = 0;
    SetRect32(0, 0, h, w);

    if (*gResult == noErr) *gResult = gFmt->advanceState();   // PS FILLS 'frame'
    if (*gResult != noErr) { sPSBuffer->Dispose(&frame); return; }

    // 2) encode RGBA → .tex
    void* outBytes = NULL; size_t outSize = 0;
    if (!EncodeTex((const unsigned char*)frame, w, h, &outBytes, &outSize)) {
        sPSBuffer->Dispose(&frame); *gResult = formatBadParameters; return;
    }
    sPSBuffer->Dispose(&frame);

    // 3) write file
    SeekStart();
    WriteSome((int32)outSize, outBytes);
    free(outBytes);

    gFmt->data = NULL;
    if (gFmt->progressProc) gFmt->progressProc(h, h);
}

static void DoWriteContinue() {}   // all work done in WriteStart (self-driving)
static void DoWriteFinish()   {}

//================================ FILTER ======================================//
static void DoFilterFile() {
    char magic[4] = {0};
    SeekStart();                  if (*gResult != noErr) return;
    ReadSome(sizeof magic, magic);if (*gResult != noErr) return;
    if (std::memcmp(magic, kTexMagic, sizeof magic) != 0)
        *gResult = formatCannotRead;     // not ours
}

//============================== ENTRY POINT ===================================//
DLLExport MACPASCAL void PluginMain(const int16     selector,
                                    FormatRecordPtr formatParamBlock,
                                    intptr_t*       data,
                                    int16*          result)
{
    try {
        // About is special: param block is an AboutRecord, not a FormatRecord.
        if (selector == formatSelectorAbout) {
            AboutRecordPtr about = reinterpret_cast<AboutRecordPtr>(formatParamBlock);
            sSPBasic = about->sSPBasic;
            // DoAbout(...) — optional
            return;
        }

        gFmt    = formatParamBlock;
        gResult = result;
        gDataH  = data;
        sSPBasic = gFmt->sSPBasic;

        if (gFmt->advanceState == NULL) { *result = errPlugInHostInsufficient; return; }

        // Opt into 32-bit coordinates (required for modern Photoshop).
        if (gFmt->HostSupports32BitCoordinates)
            gFmt->PluginUsing32BitCoordinates = true;

        AcquireSuites();

        switch (selector) {
            case formatSelectorReadPrepare:    DoReadPrepare();     break;
            case formatSelectorReadStart:      DoReadStart();       break;
            case formatSelectorReadContinue:   DoReadContinue();    break;
            case formatSelectorReadFinish:     DoReadFinish();      break;

            case formatSelectorOptionsPrepare: DoOptionsPrepare();  break;
            case formatSelectorOptionsStart:   DoOptionsStart();    break;
            case formatSelectorOptionsContinue:DoOptionsContinue(); break;
            case formatSelectorOptionsFinish:  DoOptionsFinish();   break;

            case formatSelectorEstimatePrepare:DoEstimatePrepare(); break;
            case formatSelectorEstimateStart:  DoEstimateStart();   break;
            case formatSelectorEstimateContinue:DoEstimateContinue();break;
            case formatSelectorEstimateFinish: DoEstimateFinish();  break;

            case formatSelectorWritePrepare:   DoWritePrepare();    break;
            case formatSelectorWriteStart:     DoWriteStart();      break;
            case formatSelectorWriteContinue:  DoWriteContinue();   break;
            case formatSelectorWriteFinish:    DoWriteFinish();     break;

            case formatSelectorFilterFile:     DoFilterFile();      break;
            default: break;   // ignore unimplemented advanced selectors
        }

        // Release suites at the end of each operation (or on error).
        if (selector == formatSelectorReadFinish     ||
            selector == formatSelectorWriteFinish    ||
            selector == formatSelectorOptionsFinish  ||
            selector == formatSelectorEstimateFinish ||
            selector == formatSelectorFilterFile     ||
            *gResult != noErr)
        {
            ReleaseSuites();
        }
    }
    catch (...) {
        if (result) *result = -1;
    }
}
```

Notes on the skeleton vs. the real SDK:
- `PSSDKGetFPosEnd` is illustrative — get file length via the SDK file utility or by seeking to end; or read in fixed chunks. SimpleFormat avoids needing the length because its header carries dimensions.
- Holding the whole decoded frame between ReadStart and ReadFinish is fine for textures (small). For huge images stream row-by-row like SimpleFormat (`:757-779`).
- If you'd rather store globals in the host-preserved `data` handle (SimpleFormat style), allocate a struct handle via `sPSHandle->New` in PluginMain when `*data == 0` (`SimpleFormat.cpp:240-247`) instead of file-scope statics.

---

## Summary

- **One entry point, selector-driven**: implement `PluginMain(selector, FormatRecord*, data, result)` and switch on `formatSelector*` (read: Prepare→Start→Continue→Finish; write: Options→Estimate→Write, each Prepare→Start→Continue→Finish; plus FilterFile to sniff). Pixels move through `FormatRecord.data` + the `colBytes/rowBytes/planeBytes/loPlane/hiPlane/theRect32` descriptor, pumped by `advanceState()`; file bytes move via `PSSDKRead/PSSDKWrite/PSSDKSetFPos` on `FormatRecord.dataFork`. (Gold template: `samplecode\format\simpleformat\common\SimpleFormat.cpp`; API: `photoshopapi\photoshop\PIFormat.h`.)
- **Registration is the PiPL**: declare `Kind { ImageFormat }`, `CodeWin64X86 { "PluginMain" }`, `SupportedModes { doesSupportRGBColor }`, and `ReadExtensions/WriteExtensions/FilteredExtensions { { 'tex ' } }` (extension in first 3 chars, 4th = space) plus `FormatFlags { fmtCanRead, fmtCanWrite, fmtCanWriteTransparency }`. Build x64 DLL, output `.8bi`, drop in Photoshop's Plug-ins folder. (`PIPL.r:628-890`, `SimpleFormat.r:78-146`.)
- **`.tex` ⇒ 8-bit interleaved RGBA**: `imageMode = plugInModeRGBColor (3)`, `depth = 8`, `planes = 4`, `colBytes=4, rowBytes=w*4, planeBytes=1, loPlane=0, hiPlane=3`, `transparencyMatting = 0` (straight alpha). Wire ReadStart→`DecodeTex` and WriteStart→`EncodeTex` to the BCn codec from the Paint.NET plugin.

### ⚠️ The single most important gotcha for Format plug-ins

**You MUST opt into 32-bit coordinates and then use the 32-bit fields exclusively.** In `PluginMain`, after grabbing the record, do `if (gFmt->HostSupports32BitCoordinates) gFmt->PluginUsing32BitCoordinates = true;` (`SimpleFormat.cpp:232-233`) and thereafter set/read **`imageSize32`** and **`theRect32`**, *never* the legacy 16-bit `imageSize`/`theRect`. If you forget this and write the 16-bit fields, modern Photoshop reads garbage dimensions/rectangles and the open/save silently produces a corrupt or zero-size image — the classic "it builds and registers but the pixels are wrong/empty" failure.
