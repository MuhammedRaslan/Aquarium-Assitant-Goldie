# Build and Flash Script for IoT Dashboard

## Quick Start

### 1. Build the project
```powershell
idf.py build
```

### 2. Flash everything (firmware + SPIFFS images)
```powershell
idf.py flash
```

### 3. Monitor output
```powershell
idf.py monitor
```

### Or do all at once:
```powershell
idf.py build flash monitor
```

## What Happens During Build

The build process will:
1. Compile the application code
2. **Automatically create a SPIFFS image** from the `spiffs_image/` directory
3. Include your 3 PNG animation frames in the filesystem
4. Flash both the app and SPIFFS partition to the ESP32

## SPIFFS Image Structure

```
spiffs_image/
└── lvgl_ui/
    └── images/
        ├── anim_frame_0.png (205 KB)
        ├── anim_frame_1.png (207 KB)
        └── anim_frame_2.png (213 KB)
```

This structure is mounted as `/spiffs/` on the ESP32, so:
- `spiffs_image/lvgl_ui/images/anim_frame_0.png` → `/spiffs/lvgl_ui/images/anim_frame_0.png`

## Adding More Images

1. Copy new PNG files to `spiffs_image/lvgl_ui/images/`
2. Rebuild: `idf.py build`
3. Flash: `idf.py flash`

## Partition Layout

| Partition | Type    | Size | Purpose                           |
|-----------|---------|------|-----------------------------------|
| nvs       | data    | 24KB | Non-volatile storage              |
| phy_init  | data    | 4KB  | PHY init data                     |
| factory   | app     | 6MB  | Application firmware              |
| **storage** | **spiffs** | **3MB** | **PNG images and assets** |

Total: 9MB (fits in 16MB flash)

## Troubleshooting

### If SPIFFS mount fails:
```powershell
# Erase flash and reflash
idf.py erase-flash
idf.py build flash
```

### To verify SPIFFS contents:
Check the monitor output for:
```
I (xxx) lvgl_example: SPIFFS: xxx KB total, xxx KB used
I (xxx) dashboard: Loaded initial frame: /spiffs/lvgl_ui/images/anim_frame_0.png
I (xxx) dashboard: Loading animation frame 1: /spiffs/lvgl_ui/images/anim_frame_1.png
```

### If animation doesn't show:
1. Check SPIFFS initialization succeeded
2. Verify PNG files are in `spiffs_image/lvgl_ui/images/`
3. Check file sizes aren't too large (should be < 500KB each)
4. Ensure PNG decoder is enabled: `CONFIG_LV_USE_PNG=y`

## Image Optimization Tips

To reduce PNG file sizes:
```powershell
# Using ImageMagick (if installed)
magick convert anim_frame_0.png -resize 320x480 -quality 85 anim_frame_0_opt.png

# Or use online tools:
# - TinyPNG: https://tinypng.com/
# - Squoosh: https://squoosh.app/
```

Target size: < 200KB per frame for smooth loading

## Configuration Changes Made

1. **partitions.csv**: Added 3MB SPIFFS partition
2. **sdkconfig.defaults**: Enabled SPIFFS support
3. **CMakeLists.txt**: Added SPIFFS image build step
4. **main.cpp**: Added SPIFFS initialization
5. **dashboard.cpp**: Updated to load PNGs from SPIFFS

## Next Steps

After successful flash:
1. Watch the animation cycle through your 3 frames
2. Observe the gauges updating with simulated sensor data
3. Connect real sensors and update the sensor values
4. Add more animation sequences based on sensor thresholds
