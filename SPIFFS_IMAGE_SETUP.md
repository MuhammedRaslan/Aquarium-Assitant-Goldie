# SPIFFS Image Storage Setup

## Overview

Instead of SD card, we're using SPIFFS (SPI Flash File System) which is:
- ✅ Faster than SD card
- ✅ More reliable (no card insertion issues)
- ✅ Built into ESP32-S3 flash memory
- ✅ Automatically flashed with firmware

## File Preparation

### Step 1: Convert Images to BIN Format

Use the LVGL Online Image Converter:

1. Go to: **https://lvgl.io/tools/imageconverter**

2. **CRITICAL Settings**:
   - **Color format**: `True color`
   - **Output format**: `Binary RGB565`
   - **Swap endian**: `Little endian (Default)` or try toggling if colors are wrong
   - **Image dimensions**: 480×320 pixels

3. Upload your PNG/JPG images (must be exactly 480×320)

4. Download the `.bin` files

5. **Verify file size**: Each must be **exactly 307,200 bytes**
   - If different size → Wrong format, reconvert

### Step 2: Place Files in SPIFFS Image Directory

Copy all BIN files directly to:
```
lvgl_anim/spiffs_image/
```

**NOT in subfolders!** Files should be:
```
spiffs_image/
├── frame1.bin   (307,200 bytes)
├── frame2.bin   (307,200 bytes)
├── frame3.bin   (307,200 bytes)
...
├── frame23.bin  (307,200 bytes)
└── frame24.bin  (307,200 bytes)
```

### Step 3: Build and Flash

The build system will automatically:
1. Create SPIFFS image from `spiffs_image/` folder
2. Flash it to the SPIFFS partition
3. Mount it at `/spiffs` during runtime

## File Naming Convention

### Happy Mood (Category 0)
- `frame1.bin` through `frame8.bin`

### Sad Mood (Category 1)
- `frame9.bin` through `frame16.bin`

### Angry Mood (Category 2)
- `frame17.bin` through `frame24.bin`

## Troubleshooting

### Rainbow/Wrong Colors

**Cause**: BIN files not in correct RGB565 format

**Solution**:
1. Re-convert ONE image on LVGL website
2. Try toggling "Swap endian" setting
3. Download and check file size (must be 307,200 bytes)
4. Test with just `frame1.bin` first
5. If it works, use same settings for all frames

### "Failed to open /spiffs/frameX.bin"

**Cause**: Files not in spiffs_image folder or wrong name

**Solution**:
1. Check files are in `spiffs_image/` (not in subdirectories)
2. Check naming: `frame1.bin` not `Frame1.bin` or `frame_1.bin`
3. Rebuild project (ESP-IDF rebuilds SPIFFS image)

### Lag/Stuttering

**Cause**: Files too large or wrong format

**Solution**:
1. Verify each file is exactly 307,200 bytes
2. Use Binary RGB565 format (not RGB888)
3. Don't use compression in LVGL converter

## Testing Individual Files

To test if your BIN files are correct format:

1. **Check file size**:
   ```
   480 × 320 × 2 = 307,200 bytes exactly
   ```

2. **Test one frame**: Place only `frame1.bin` in `spiffs_image/`

3. **Flash and run**: Monitor should show:
   ```
   SPIFFS accessible - frame files found!
   Initial frames loaded and ready
   ```

4. **If frame displays correctly**: Convert rest with same settings

## LVGL Converter Settings Details

**For CORRECT colors**:
- Color format: **True color**
- Output format: **Binary RGB565**
- Dithering: Optional (improves gradients)
- Compression: **None**

**Test both endianness if colors wrong**:
- First try: Little endian (default)
- If rainbow: Toggle to Big endian
- One setting will work correctly

## Memory Usage

- **SPIFFS Partition**: 3 MB (defined in partitions.csv)
- **Each BIN file**: ~300 KB
- **Total 24 frames**: ~7.2 MB (won't fit, use 8-10 frames max)

### Recommendation: Start with 8 frames per category

Initially test with:
- 8 Happy frames (frame1.bin - frame8.bin)
- Can add more later if space permits

Or reduce to smaller images (e.g., 320×240) for more frames.
