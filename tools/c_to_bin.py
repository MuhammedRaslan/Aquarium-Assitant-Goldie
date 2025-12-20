#!/usr/bin/env python3
"""
Convert LVGL C array image files to raw binary files.
Extracts RGB565 pixel data from C array declarations and saves as .bin files.
"""

import re
import sys
import os
from pathlib import Path

def parse_c_array(c_file_path):
    """
    Parse a C file containing LVGL image data and extract the pixel array.
    Returns the pixel data as bytes.
    """
    with open(c_file_path, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    
    # Find the pixel data array (usually named like frame1_map or similar)
    # Look for pattern: static const uint8_t ARRAYNAME[] = { ... };
    array_pattern = r'static\s+const\s+(?:LV_ATTRIBUTE_MEM_ALIGN\s+)?uint(?:8|16)_t\s+\w+_map\[\]\s*=\s*\{([^}]+)\}'
    match = re.search(array_pattern, content, re.DOTALL)
    
    if not match:
        # Try alternative pattern without _map suffix
        array_pattern = r'static\s+const\s+(?:LV_ATTRIBUTE_MEM_ALIGN\s+)?uint(?:8|16)_t\s+\w+\[\]\s*=\s*\{([^}]+)\}'
        match = re.search(array_pattern, content, re.DOTALL)
    
    if not match:
        raise ValueError(f"Could not find pixel data array in {c_file_path}")
    
    # Extract array contents
    array_str = match.group(1)
    
    # Parse hex values (0x00 format)
    hex_values = re.findall(r'0x([0-9a-fA-F]{2})', array_str)
    
    if not hex_values:
        raise ValueError(f"Could not parse hex values from array in {c_file_path}")
    
    # Convert to bytes
    pixel_data = bytes([int(val, 16) for val in hex_values])
    
    print(f"  Extracted {len(pixel_data)} bytes from {c_file_path}")
    return pixel_data

def convert_c_to_bin(c_file_path, output_dir):
    """
    Convert a single C file to BIN file.
    """
    c_path = Path(c_file_path)
    
    if not c_path.exists():
        print(f"Error: File not found: {c_file_path}")
        return False
    
    try:
        # Parse the C file
        pixel_data = parse_c_array(c_path)
        
        # Generate output filename (frame1.c -> frame1.bin)
        bin_filename = c_path.stem + '.bin'
        bin_path = Path(output_dir) / bin_filename
        
        # Create output directory if needed
        bin_path.parent.mkdir(parents=True, exist_ok=True)
        
        # Write binary file
        with open(bin_path, 'wb') as f:
            f.write(pixel_data)
        
        print(f"✓ Created {bin_path} ({len(pixel_data)} bytes)")
        return True
        
    except Exception as e:
        print(f"✗ Failed to convert {c_path}: {e}")
        return False

def main():
    """
    Main conversion function.
    Usage: python c_to_bin.py [input_dir] [output_dir]
    """
    # Default paths
    script_dir = Path(__file__).parent
    project_dir = script_dir.parent
    input_dir = project_dir / 'components' / 'lvgl_ui'
    output_dir = project_dir / 'sd_card_files' / 'frames'
    
    # Allow command-line override
    if len(sys.argv) >= 2:
        input_dir = Path(sys.argv[1])
    if len(sys.argv) >= 3:
        output_dir = Path(sys.argv[2])
    
    print("=" * 60)
    print("LVGL C Array to BIN Converter")
    print("=" * 60)
    print(f"Input directory:  {input_dir}")
    print(f"Output directory: {output_dir}")
    print()
    
    # Find all frame*.c files
    c_files = sorted(input_dir.glob('frame*.c'))
    
    if not c_files:
        print(f"Error: No frame*.c files found in {input_dir}")
        return 1
    
    print(f"Found {len(c_files)} frame files to convert:")
    for f in c_files:
        print(f"  - {f.name}")
    print()
    
    # Convert each file
    success_count = 0
    for c_file in c_files:
        if convert_c_to_bin(c_file, output_dir):
            success_count += 1
    
    print()
    print("=" * 60)
    print(f"Conversion complete: {success_count}/{len(c_files)} successful")
    print("=" * 60)
    print()
    print("Next steps:")
    print(f"1. Copy all .bin files from '{output_dir}' to your SD card")
    print("2. Create a 'frames' folder on the SD card root")
    print("3. Place all frame*.bin files in /frames/ directory on SD card")
    print("4. Rebuild and flash your ESP32 project")
    print()
    
    return 0 if success_count == len(c_files) else 1

if __name__ == '__main__':
    sys.exit(main())
