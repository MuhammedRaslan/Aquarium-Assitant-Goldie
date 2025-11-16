#!/usr/bin/env python3
"""
Convert PNG images to LVGL C arrays
Requires: pip install pillow
"""

from PIL import Image
import sys
import os

def convert_png_to_c(png_path, output_path, var_name):
    """Convert PNG to LVGL C array format"""
    
    # Open image
    img = Image.open(png_path)
    
    # Convert to RGB565 format
    img = img.convert('RGB')
    width, height = img.size
    
    # Open output file
    with open(output_path, 'w') as f:
        # Write header
        f.write(f'#include "lvgl.h"\n\n')
        f.write(f'#ifndef LV_ATTRIBUTE_MEM_ALIGN\n')
        f.write(f'#define LV_ATTRIBUTE_MEM_ALIGN\n')
        f.write(f'#endif\n\n')
        
        # Write pixel data array
        f.write(f'const LV_ATTRIBUTE_MEM_ALIGN uint8_t {var_name}_map[] = {{\n')
        
        pixel_data = []
        for y in range(height):
            for x in range(width):
                r, g, b = img.getpixel((x, y))
                # Convert to RGB565
                r5 = (r >> 3) & 0x1F
                g6 = (g >> 2) & 0x3F
                b5 = (b >> 3) & 0x1F
                rgb565 = (r5 << 11) | (g6 << 5) | b5
                # Little endian
                pixel_data.append(f'0x{rgb565 & 0xFF:02x}')
                pixel_data.append(f'0x{(rgb565 >> 8) & 0xFF:02x}')
        
        # Write data in rows of 16 bytes
        for i in range(0, len(pixel_data), 16):
            row = pixel_data[i:i+16]
            f.write('  ' + ', '.join(row))
            if i + 16 < len(pixel_data):
                f.write(',\n')
            else:
                f.write('\n')
        
        f.write('};\n\n')
        
        # Write image descriptor
        f.write(f'const lv_img_dsc_t {var_name} = {{\n')
        f.write(f'  .header.always_zero = 0,\n')
        f.write(f'  .header.w = {width},\n')
        f.write(f'  .header.h = {height},\n')
        f.write(f'  .data_size = {len(pixel_data)},\n')
        f.write(f'  .header.cf = LV_IMG_CF_TRUE_COLOR,\n')
        f.write(f'  .data = {var_name}_map,\n')
        f.write(f'}};\n')
    
    print(f"Converted {png_path} -> {output_path}")
    print(f"  Size: {width}x{height}, Data: {len(pixel_data)} bytes")

if __name__ == "__main__":
    # Convert all 3 frames
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    img_dir = os.path.join(base_dir, "components", "lvgl_ui", "images")
    
    for i in range(3):
        png_file = os.path.join(img_dir, f"anim_frame_{i}.png")
        c_file = os.path.join(img_dir, f"anim_frame_{i}.c")
        var_name = f"anim_frame_{i}"
        
        if os.path.exists(png_file):
            convert_png_to_c(png_file, c_file, var_name)
        else:
            print(f"Warning: {png_file} not found")
