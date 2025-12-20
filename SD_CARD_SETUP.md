# SD Card BIN File Setup Guide

## Converting Images to BIN Format

### Using LVGL Online Image Converter

1. **Go to**: https://lvgl.io/tools/imageconverter

2. **Settings**:
   - **Color format**: `True color`
   - **Output format**: `Binary RGB565`
   - **Byte order**: Check your system (usually little-endian)
   - **Name**: `frame1`, `frame2`, etc. (doesn't matter for BIN)

3. **Upload** your PNG/JPG images (480×320 pixels each)

4. **Download** the resulting `.bin` files

5. **Rename** them to:
   - `frame1.bin`
   - `frame2.bin`
   - `frame3.bin`
   - ... (up to frame24.bin)

## Expected File Size

Each 480×320 RGB565 image should be:
- **307,200 bytes** (480 × 320 × 2 bytes)
- **~300 KB** per frame

## SD Card Directory Structure

Create this folder structure on your SD card:

```
SD Card Root/
└── frames/
    ├── frame1.bin   (Happy - Frame 1)
    ├── frame2.bin   (Happy - Frame 2)
    ├── frame3.bin   (Happy - Frame 3)
    ├── frame4.bin   (Happy - Frame 4)
    ├── frame5.bin   (Happy - Frame 5)
    ├── frame6.bin   (Happy - Frame 6)
    ├── frame7.bin   (Happy - Frame 7)
    ├── frame8.bin   (Happy - Frame 8)
    ├── frame9.bin   (Sad - Frame 1)
    ├── frame10.bin  (Sad - Frame 2)
    ├── frame11.bin  (Sad - Frame 3)
    ├── frame12.bin  (Sad - Frame 4)
    ├── frame13.bin  (Sad - Frame 5)
    ├── frame14.bin  (Sad - Frame 6)
    ├── frame15.bin  (Sad - Frame 7)
    ├── frame16.bin  (Sad - Frame 8)
    ├── frame17.bin  (Angry - Frame 1)
    ├── frame18.bin  (Angry - Frame 2)
    ├── frame19.bin  (Angry - Frame 3)
    ├── frame20.bin  (Angry - Frame 4)
    ├── frame21.bin  (Angry - Frame 5)
    ├── frame22.bin  (Angry - Frame 6)
    ├── frame23.bin  (Angry - Frame 7)
    └── frame24.bin  (Angry - Frame 8)
```

## Animation Categories

The system supports 3 animation moods:

- **Category 0 - Happy**: Frames 1-8
- **Category 1 - Sad**: Frames 9-16
- **Category 2 - Angry**: Frames 17-24

### Switching Categories in Code

```cpp
// Switch to Happy animation
dashboard_set_animation_category(0);

// Switch to Sad animation
dashboard_set_animation_category(1);

// Switch to Angry animation
dashboard_set_animation_category(2);

// Get current category
uint8_t current = dashboard_get_animation_category();
```

## Steps to Deploy

1. **Format SD card** (FAT32 recommended)

2. **Create folder**: Make a folder called `frames` in the root

3. **Copy BIN files**: Place all your `.bin` files into the `frames/` folder

4. **Insert SD card** into your ESP32-S3 device

5. **Build and flash** the updated firmware

## Adding More Frames

To add more animation categories or frames:

1. Convert additional images to BIN format

2. Name them sequentially (e.g., `frame25.bin`, `frame26.bin`, etc.)

3. Copy to SD card `/frames/` folder

4. Update constants in `dashboard.cpp`:
   ```cpp
   #define FRAMES_PER_CATEGORY 8  // Frames per mood
   #define TOTAL_CATEGORIES 4     // If adding 4th category
   #define TOTAL_FRAMES 32        // Update accordingly
   ```

5. Rebuild and flash

## Troubleshooting

### Images don't display
- Check serial monitor for error messages
- Verify SD card is mounted at `/sdcard`
- Confirm files are exactly 307,200 bytes each
- Ensure folder is named `frames` (lowercase)

### "Failed to open" errors
- Check file naming: `frame1.bin` not `Frame1.bin`
- Verify SD card filesystem (FAT32)
- Check SD card connections

### Wrong colors/garbled images
- Ensure RGB565 format was used (not RGB888)
- Verify image dimensions are 480×320
- Check byte order (should be little-endian)

## Memory Usage

- **RAM**: 307,200 bytes in PSRAM (one frame buffer)
- **Flash**: frame*.c files removed from build (saves ~2.4 MB)
- **SD Card**: ~300 KB per frame (unlimited storage)

## LVGL Converter Settings Detail

When converting on lvgl.io/tools/imageconverter:

- **Image source**: Upload your 480×320 PNG/JPG
- **Color format**: True color
- **Output format**: Binary RGB565 ✓
- **Dithering**: (optional, improves gradients)
- **Compression**: None (for fastest loading)

Click "Convert" and download the `.bin` file.
