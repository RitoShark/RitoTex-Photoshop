# TEX Save Implementation Plan

## Overview
This document provides a step-by-step plan to implement TEX file saving functionality in the Photoshop plugin. Follow this if context is lost or implementation needs to be continued.

---

## Phase 1: Core TEX Save (CURRENT PHASE)

### Step 1: Add TEX Format Detection in Write Flow
**File**: `IntelCompressionPlugin/IntelPlugin.cpp`

**Location**: After `DoWriteStart()` function (around line 2300)

**Action**: Add detection for TEX vs DDS save format

```cpp
// In DoWriteFinish() or DoWriteContinue()
// Add check for file extension to determine format
bool isTEXFormat = false;
if (ps.formatRecord->fileSpec) {
    // Check file extension
    char filename[256];
    // Get filename from fileSpec
    // if ends with ".tex" set isTEXFormat = true
}

if (isTEXFormat) {
    DoWriteTEX();  // New function
} else {
    DoWriteDDS();  // Existing function
}
```

### Step 2: Create TEX Header Builder
**File**: `IntelCompressionPlugin/IntelPlugin.cpp`

**Location**: Before `DoWriteTEX()` function

**Action**: Add helper function to build TEX header

```cpp
TEX_HEADER BuildTEXHeader(uint16_t width, uint16_t height,
                          tex_format format, bool hasMipmaps)
{
    TEX_HEADER header;
    memcpy(header.magic, "TEX\0", 4);
    header.image_width = width;
    header.image_height = height;
    header.unk1 = 0;  // Unknown field - use 0 for now
    header.tex_format = format;
    header.unk2 = 0;  // Unknown field - use 0 for now
    header.has_mipmaps = hasMipmaps;
    return header;
}
```

### Step 3: Map Compression Types to TEX Formats
**File**: `IntelCompressionPlugin/IntelPlugin.cpp`

**Location**: Near `BuildTEXHeader()` function

**Action**: Add mapping function

```cpp
tex_format GetTEXFormatFromCompression(DXGI_FORMAT dxgiFormat)
{
    switch (dxgiFormat) {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return tex_format_dxt1;

        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            return tex_format_dxt5;

        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return tex_format_bgra8;

        default:
            // Fallback or error
            return tex_format_dxt5;
    }
}
```

### Step 4: Implement DoWriteTEX() Main Function
**File**: `IntelCompressionPlugin/IntelPlugin.cpp`

**Location**: After `DoWriteDDS()` function (around line 2200)

**Action**: Create main TEX write function

```cpp
void IntelPlugin::DoWriteTEX()
{
    showLoadingCursor();

    // 1. Get image dimensions
    uint16_t width = static_cast<uint16_t>(ps.formatRecord->imageSize.h);
    uint16_t height = static_cast<uint16_t>(ps.formatRecord->imageSize.v);

    // 2. Determine format
    tex_format texFormat = GetTEXFormatFromCompression(ps.data->encoding_g);

    // 3. Check if mipmaps should be generated
    bool hasMipmaps = (ps.data->MipMapTypeIndex != MipmapEnum::NONE);

    // 4. Build TEX header
    TEX_HEADER header = BuildTEXHeader(width, height, texFormat, hasMipmaps);

    // 5. Compress image data (reuse existing compression)
    DirectX::ScratchImage *compressedImage = nullptr;
    DirectX::ScratchImage *uncompressedImage = nullptr;

    bool hasAlpha = HasAlpha();
    bool result = CompressToScratchImage(&compressedImage, &uncompressedImage, hasAlpha);

    if (!result || !compressedImage) {
        errorMessage("Failed to compress image for TEX export", "TEX Save Error");
        showNormalCursor();
        SetResult(writErr);
        return;
    }

    // 6. Write TEX file
    OSErr err = WriteTEXFile(header, compressedImage);

    // 7. Cleanup
    if (compressedImage) delete compressedImage;
    if (uncompressedImage) delete uncompressedImage;

    showNormalCursor();
    SetResult(err);
}
```

### Step 5: Implement WriteTEXFile() Helper
**File**: `IntelCompressionPlugin/IntelPlugin.cpp`

**Location**: Before `DoWriteTEX()`

**Action**: Create file writing function

```cpp
OSErr IntelPlugin::WriteTEXFile(const TEX_HEADER& header,
                                DirectX::ScratchImage* compressedImage)
{
    // 1. Open file for writing
    OSErr err = PSSDKSetFPos(ps.formatRecord->dataFork, fsFromStart, 0);
    if (err != noErr) {
        errorMessage("Failed to set file position", "TEX Save Error");
        return err;
    }

    // 2. Write TEX header
    int32 headerSize = sizeof(TEX_HEADER);
    err = PSSDKWrite(ps.formatRecord->dataFork, &headerSize, &header);
    if (err != noErr) {
        errorMessage("Failed to write TEX header", "TEX Save Error");
        return err;
    }

    // 3. Write mipmaps in REVERSE order (smallest to largest)
    if (header.has_mipmaps) {
        err = WriteTEXMipmaps(compressedImage);
        if (err != noErr) return err;
    }

    // 4. Write main image (mip level 0)
    const DirectX::Image* mainImage = compressedImage->GetImage(0, 0, 0);
    if (!mainImage) {
        errorMessage("Failed to get main image data", "TEX Save Error");
        return writErr;
    }

    int32 dataSize = static_cast<int32>(mainImage->slicePitch);
    err = PSSDKWrite(ps.formatRecord->dataFork, &dataSize, mainImage->pixels);
    if (err != noErr) {
        errorMessage("Failed to write TEX image data", "TEX Save Error");
        return err;
    }

    return noErr;
}
```

### Step 6: Implement WriteTEXMipmaps() Helper
**File**: `IntelCompressionPlugin/IntelPlugin.cpp`

**Location**: Before `WriteTEXFile()`

**Action**: Create mipmap writing function

```cpp
OSErr IntelPlugin::WriteTEXMipmaps(DirectX::ScratchImage* compressedImage)
{
    size_t mipCount = compressedImage->GetMetadata().mipLevels;

    if (mipCount <= 1) {
        return noErr; // No mipmaps to write
    }

    // Write mipmaps in REVERSE order (from smallest to largest)
    // Skip mip 0 (main image) - write from mipCount-1 down to 1
    for (int mipLevel = static_cast<int>(mipCount) - 1; mipLevel >= 1; mipLevel--) {
        const DirectX::Image* mipImage = compressedImage->GetImage(mipLevel, 0, 0);
        if (!mipImage) {
            errorMessage("Failed to get mipmap data", "TEX Save Error");
            return writErr;
        }

        int32 mipSize = static_cast<int32>(mipImage->slicePitch);
        OSErr err = PSSDKWrite(ps.formatRecord->dataFork, &mipSize, mipImage->pixels);
        if (err != noErr) {
            errorMessage("Failed to write mipmap data", "TEX Save Error");
            return err;
        }
    }

    return noErr;
}
```

### Step 7: Add Function Declarations to Header
**File**: `IntelCompressionPlugin/IntelPlugin.h`

**Location**: In private section of IntelPlugin class (around line 330)

**Action**: Add declarations

```cpp
private:
    // ... existing functions ...

    void DoWriteTEX();
    OSErr WriteTEXFile(const TEX_HEADER& header, DirectX::ScratchImage* compressedImage);
    OSErr WriteTEXMipmaps(DirectX::ScratchImage* compressedImage);
```

### Step 8: Update File Type Filter
**File**: `IntelCompressionPlugin/SaveOptionsDialog.cpp` or relevant UI file

**Location**: Where file save dialog is configured

**Action**: Add TEX file type option

```cpp
// Add to file type filter
"TEX Files (*.tex)|*.tex|DDS Files (*.dds)|*.dds|All Files (*.*)|*.*"
```

---

## Testing Checklist

### Basic TEX Save Tests
- [ ] Save simple texture as TEX (no mipmaps, DXT1)
- [ ] Open saved TEX and verify it loads correctly
- [ ] Save texture with alpha as TEX (DXT5)
- [ ] Save texture with mipmaps as TEX
- [ ] Verify mipmap order (smallest first in file)

### Edge Cases
- [ ] Save 1x1 texture
- [ ] Save non-power-of-2 texture
- [ ] Save very large texture (4096x4096)
- [ ] Handle save errors gracefully

### Format Tests
- [ ] BC1/DXT1 format
- [ ] BC3/DXT5 format
- [ ] With and without mipmaps
- [ ] SRGB variants

---

## Validation After Implementation

### Code Review Checklist
- [ ] All error messages are descriptive
- [ ] Memory is properly cleaned up (no leaks)
- [ ] File handles are properly closed
- [ ] Mipmap order is correct (reverse)
- [ ] Header validation before writing
- [ ] Block size calculations are correct

### Integration Checklist
- [ ] Doesn't break existing DDS save
- [ ] Preview still works
- [ ] Compression settings apply correctly
- [ ] UI shows correct format options

---

## Phase 2: Validation & Error Handling (NEXT)

### Tasks
1. Add `ValidateTEXHeader()` function
2. Replace all `errorMessage(LINE_STRING, "Err")` with descriptive messages
3. Add bounds checking for image dimensions
4. Validate format compatibility before save
5. Add progress reporting for large files

### Files to Modify
- `IntelPlugin.cpp` - Add validation functions
- All error messages throughout codebase

---

## Phase 3: Documentation (AFTER TESTING)

### Tasks
1. Update README.md with TEX format documentation
2. Add code comments to TEX functions
3. Create CHANGELOG.md entry
4. Document TEX file structure
5. Add usage examples

---

## Rollback Plan

If implementation fails:
1. Current code is in git - can revert commits
2. DDS save is untouched - still works
3. TEX load functionality preserved
4. Can disable TEX save in UI if needed

---

## Performance Optimization (FUTURE)

### Potential Improvements
- Multi-threaded compression
- Streaming write for large files
- Memory-mapped file I/O
- Compression quality settings

---

## Known Issues to Address

1. **Unknown Fields**: `unk1` and `unk2` in TEX header - purpose unclear, using 0
2. **Format Support**: Only DXT1/DXT5 initially, expand later
3. **Validation**: Minimal validation in current load code
4. **Error Messages**: Many generic error messages need improvement

---

## Quick Reference

### Key File Locations
- **Main logic**: `IntelCompressionPlugin/IntelPlugin.cpp`
- **Header defs**: `IntelCompressionPlugin/IntelPlugin.h`
- **UI dialog**: `IntelCompressionPlugin/SaveOptionsDialog.cpp`
- **Decompression**: `IntelCompressionPlugin/s3tc.cpp`

### Key Functions to Study
- `DoWriteDDS()` - Template for TEX save
- `CompressToScratchImage()` - Compression pipeline
- `DoReadStart()` - TEX loading (reverse of save)

### Build & Test
```bash
# Build
Open IntelTextureWorks.sln in Visual Studio
Build > Build Solution

# Install
Copy Plugins/x64/IntelTextureWorks.8bi to:
C:\Program Files\Adobe\Adobe Photoshop [version]\Plug-ins\File Formats\

# Test
1. Open Photoshop
2. Open test image
3. File > Save As
4. Select TEX format
5. Configure compression options
6. Save
7. File > Open As > TEX
8. Verify image loads correctly
```

---

*Implementation Start Date: 2026-02-24*
*Status: Phase 1 - Step 1*
