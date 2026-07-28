#!/usr/bin/env python3
"""
Generate a fully-unlocked Diddy Kong Racing save file.

Writes all-FF data to the three SaveFile bitstreams (blocks 0-4, 5-9, 10-14)
with valid checksums, and writes a fully unlocked SaveConfig to block 15. The
SaveFile bitstream marks every race as cleared+silver, all bosses defeated,
all trophies/balloons/amulets collected, all hub doors opened, all keys and
cutscene flags set. The SaveConfig sets Adventure 2, Drumstick, all 20 TT
courses, and subtitles.

Usage:
    ./scripts/unlock_save.py [--path SAVE_PATH]

If --path is omitted the script targets
~/.config/DiddyKongRacingRecompiled/saves/dkr-us-v80.bin.

The script overwrites the save file in place; the game overwrites the file
on the next save, so re-run as needed.
"""

import argparse
import os
import struct
import sys


EEPROM_BLOCK_SIZE = 8
NUMBER_OF_SAVE_FILES = 3
SAVE_BLOCKS_PER_FILE = 5
SAVE_FILE_SIZE = EEPROM_BLOCK_SIZE * SAVE_BLOCKS_PER_FILE  # 40 bytes
SAVE_BUFFER_SIZE = 0x800  # 2 KiB (Eep16k)
CONFIG_BLOCK_INDEX = 3 * SAVE_BLOCKS_PER_FILE  # 15
CONFIG_BLOCK_OFFSET = CONFIG_BLOCK_INDEX * EEPROM_BLOCK_SIZE  # 120


def save_file_checksum(save_data: bytes) -> int:
    """The decomp's SaveFile checksum: 5 + sum(bytes[2..39]) mod 2^16."""
    return (5 + sum(save_data[2:])) & 0xFFFF


def build_unlocked_save_file() -> bytes:
    """Return a 40-byte SaveFile payload with every bit set and a valid
    checksum. The decomp's populate_settings_from_save_data treats a payload
    where the initial 16-bit checksum equals 5 + sum(bytes[2..39]) and all
    subsequent bits are 1 as a fully completed save."""
    data = bytearray(SAVE_FILE_SIZE)
    for i in range(SAVE_FILE_SIZE):
        data[i] = 0xFF
    checksum = save_file_checksum(data)
    data[0] = (checksum >> 8) & 0xFF
    data[1] = checksum & 0xFF
    return bytes(data)


def eeprom_settings_checksum(value: int) -> int:
    """Sum of the 14 low nibbles of a 56-bit value plus 5, matching the
    decomp's calculate_eeprom_settings_checksum."""
    checksum = 5
    for i in range(14):
        checksum += (value >> (i * 4)) & 0xF
    return checksum & 0xFF


def build_unlocked_config() -> bytes:
    """Return the 8-byte SaveConfig payload (block 15) with every
    character/TT unlock bit set plus a valid 8-bit checksum at the top byte.

    Bit layout (matching Diddy-Kong-Racing/include/save_layout.h):
      bit 0       unlockedAdv2
      bit 1       unlockedDrumstick
      bits 2-3    language (0 = English)
      bits 4-23   TT_COURSES (20 courses, one bit each)
      bit 24      subtitles
      bits 25-55  reserved
      bits 56-63  checksum (lower 56 bits fed into
                          calculate_eeprom_settings_checksum)
    """
    value = (
        (1 << 0)         # unlockedAdv2
        | (1 << 1)       # unlockedDrumstick
        | (0 << 2)       # language = English
        | (0xFFFFF << 4) # TT_COURSES all 20 courses
        | (1 << 24)      # subtitles
    )
    value |= eeprom_settings_checksum(value) << 56
    return struct.pack('<Q', value)


def write_unlocked_save(path: str) -> None:
    if os.path.exists(path):
        with open(path, 'rb') as f:
            existing = bytearray(f.read())
    else:
        existing = bytearray(SAVE_BUFFER_SIZE)
    if len(existing) < SAVE_BUFFER_SIZE:
        existing.extend(b'\x00' * (SAVE_BUFFER_SIZE - len(existing)))

    save_block = build_unlocked_save_file()
    for slot in range(NUMBER_OF_SAVE_FILES):
        offset = slot * SAVE_FILE_SIZE
        existing[offset:offset + SAVE_FILE_SIZE] = save_block

    existing[CONFIG_BLOCK_OFFSET:CONFIG_BLOCK_OFFSET + EEPROM_BLOCK_SIZE] = (
        build_unlocked_config()
    )

    with open(path, 'wb') as f:
        f.write(existing)


def default_save_path() -> str:
    config_dir = os.environ.get(
        'DKR_SAVE_DIR',
        os.path.expanduser('~/.config/DiddyKongRacingRecompiled/saves'),
    )
    return os.path.join(config_dir, 'dkr-us-v80.bin')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--path',
        default=default_save_path(),
        help='Path to the save file (default: %(default)s)',
    )
    args = parser.parse_args()

    write_unlocked_save(args.path)
    print(f'Wrote unlocked save to {args.path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
