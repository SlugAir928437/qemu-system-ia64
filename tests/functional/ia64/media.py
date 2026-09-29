"""Deterministic synthetic boot media for the IA-64 firmware tests."""

# SPDX-License-Identifier: GPL-2.0-or-later

from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
import uuid
import zlib


SECTOR_SIZE = 512
ISO_SECTOR_SIZE = 2048
DISK_SECTORS = 16384
FAT32_DISK_SECTORS = 70000
FAT_SECTORS = 64
FAT32_SECTORS = 600
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR_SIZE
DATA_START = 1 + FAT_SECTORS + ROOT_SECTORS
TEST_BLOCKS = 256
GPT_ESP_START = 63
MBR_ESP_START = 63


@dataclass(frozen=True)
class MediaInfo:
    path: Path
    sha256: str
    test_offset: int | None = None
    test_data: bytes | None = None


def crc32(data: bytes | bytearray | memoryview) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def crc16_ccitt(data: bytes | bytearray | memoryview) -> int:
    value = 0
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = (((value << 1) ^ 0x1021) if value & 0x8000
                     else value << 1) & 0xFFFF
    return value


def file_sha256(path: str | Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _set_fat_entry(fat: memoryview, cluster: int, value: int,
                   fat12: bool = False) -> None:
    if not fat12:
        struct.pack_into("<H", fat, cluster * 2, value)
        return
    offset = cluster + cluster // 2
    current = fat[offset] | (fat[offset + 1] << 8)
    if cluster & 1:
        current = (current & 0x000F) | ((value & 0x0FFF) << 4)
    else:
        current = (current & 0xF000) | (value & 0x0FFF)
    fat[offset] = current & 0xFF
    fat[offset + 1] = current >> 8


def _directory_entry(name: bytes, attributes: int, cluster: int,
                     size: int = 0) -> bytes:
    if len(name) != 11:
        raise ValueError("FAT short name must contain exactly 11 bytes")
    entry = bytearray(32)
    entry[0:11] = name
    entry[11] = attributes
    struct.pack_into("<H", entry, 26, cluster)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def _pack_both_endian_16(buffer: memoryview | bytearray, offset: int,
                         value: int) -> None:
    struct.pack_into("<H", buffer, offset, value)
    struct.pack_into(">H", buffer, offset + 2, value)


def _pack_both_endian_32(buffer: memoryview | bytearray, offset: int,
                         value: int) -> None:
    struct.pack_into("<I", buffer, offset, value)
    struct.pack_into(">I", buffer, offset + 4, value)


def _iso_directory_record(extent_lba: int, data_length: int,
                          identifier: bytes) -> bytes:
    length = (33 + len(identifier) + 1) & ~1
    record = bytearray(length)
    record[0] = length
    _pack_both_endian_32(record, 2, extent_lba)
    _pack_both_endian_32(record, 10, data_length)
    record[18:25] = bytes((124, 1, 1, 0, 0, 0, 0))
    record[25] = 0x02
    _pack_both_endian_16(record, 28, 1)
    record[32] = len(identifier)
    record[33:33 + len(identifier)] = identifier
    return bytes(record)


def _write_iso9660(image: bytearray, volume_sectors: int,
                   path_table_lba: int, root_lba: int) -> None:
    """Write a small but complete ECMA-119 primary volume."""
    pvd = _iso_sector(image, 16)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b"QEMU IA64".ljust(32, b" ")
    pvd[40:72] = b"IA64_TEST".ljust(32, b" ")
    _pack_both_endian_32(pvd, 80, volume_sectors)
    _pack_both_endian_16(pvd, 120, 1)
    _pack_both_endian_16(pvd, 124, 1)
    _pack_both_endian_16(pvd, 128, ISO_SECTOR_SIZE)
    _pack_both_endian_32(pvd, 132, 10)
    struct.pack_into("<I", pvd, 140, path_table_lba)
    struct.pack_into(">I", pvd, 148, path_table_lba + 1)
    root_record = _iso_directory_record(root_lba, ISO_SECTOR_SIZE, b"\x00")
    pvd[156:156 + len(root_record)] = root_record
    pvd[190:318] = b"IA64_TEST".ljust(128, b" ")
    pvd[318:446] = b"QEMU".ljust(128, b" ")
    pvd[446:574] = b"QEMU".ljust(128, b" ")
    pvd[574:702] = b"QEMU IA64 TEST MEDIA".ljust(128, b" ")
    timestamp = b"2024010100000000\x00"
    pvd[813:830] = timestamp
    pvd[830:847] = timestamp
    pvd[864:881] = timestamp
    pvd[881] = 1

    little_path = _iso_sector(image, path_table_lba)
    little_path[0] = 1
    struct.pack_into("<I", little_path, 2, root_lba)
    struct.pack_into("<H", little_path, 6, 1)
    big_path = _iso_sector(image, path_table_lba + 1)
    big_path[0] = 1
    struct.pack_into(">I", big_path, 2, root_lba)
    struct.pack_into(">H", big_path, 6, 1)

    root = _iso_sector(image, root_lba)
    dot = _iso_directory_record(root_lba, ISO_SECTOR_SIZE, b"\x00")
    dotdot = _iso_directory_record(root_lba, ISO_SECTOR_SIZE, b"\x01")
    root[:len(dot)] = dot
    root[len(dot):len(dot) + len(dotdot)] = dotdot


def _fat_volume(efi_image: bytes, sectors: int = DISK_SECTORS,
                fat_sectors: int = FAT_SECTORS,
                root_entries: int = ROOT_ENTRIES,
                reserve_test_region: bool = False,
                extra_boot_files: tuple[tuple[bytes, bytes], ...] = ()) \
        -> tuple[bytearray, int]:
    root_sectors = root_entries * 32 // SECTOR_SIZE
    data_start = 1 + fat_sectors + root_sectors
    image = bytearray(sectors * SECTOR_SIZE)

    image[0:3] = b"\xeb\x3c\x90"
    image[3:11] = b"QEMUIA64"
    struct.pack_into("<H", image, 11, SECTOR_SIZE)
    image[13] = 1
    struct.pack_into("<H", image, 14, 1)
    image[16] = 1
    struct.pack_into("<H", image, 17, root_entries)
    if sectors <= 0xFFFF:
        struct.pack_into("<H", image, 19, sectors)
    else:
        struct.pack_into("<I", image, 32, sectors)
    image[21] = 0xF8
    struct.pack_into("<H", image, 22, fat_sectors)
    struct.pack_into("<H", image, 24, 32)
    struct.pack_into("<H", image, 26, 64)
    image[38] = 0x29
    image[39:43] = b"IA64"
    image[43:54] = b"IA64 TEST  "
    image[510:512] = b"\x55\xaa"

    fat_start = SECTOR_SIZE
    fat = memoryview(image)[fat_start:fat_start + fat_sectors * SECTOR_SIZE]
    fat12 = sectors - data_start < 4085
    image[54:62] = b"FAT12   " if fat12 else b"FAT16   "
    end_of_chain = 0x0FFF if fat12 else 0xFFFF
    _set_fat_entry(fat, 0, 0x0FF8 if fat12 else 0xFFF8, fat12)
    _set_fat_entry(fat, 1, end_of_chain, fat12)
    _set_fat_entry(fat, 2, end_of_chain, fat12)
    _set_fat_entry(fat, 3, end_of_chain, fat12)

    file_clusters = (len(efi_image) + SECTOR_SIZE - 1) // SECTOR_SIZE
    first_file_cluster = 4
    last_file_cluster = first_file_cluster + file_clusters - 1
    reserved = TEST_BLOCKS if reserve_test_region else 0
    for cluster in range(first_file_cluster, last_file_cluster + 1):
        next_cluster = (end_of_chain if cluster == last_file_cluster
                        else cluster + 1)
        _set_fat_entry(fat, cluster, next_cluster, fat12)

    extra_records = []
    next_free_cluster = last_file_cluster + 1
    for short_name, contents in extra_boot_files:
        if len(short_name) != 11:
            raise ValueError("extra FAT short name must contain 11 bytes")
        cluster_count = max(1, (len(contents) + SECTOR_SIZE - 1) //
                            SECTOR_SIZE)
        first_cluster = next_free_cluster
        last_cluster = first_cluster + cluster_count - 1
        for cluster in range(first_cluster, last_cluster + 1):
            _set_fat_entry(
                fat, cluster,
                end_of_chain if cluster == last_cluster else cluster + 1,
                fat12)
        extra_records.append((short_name, contents, first_cluster))
        next_free_cluster = last_cluster + 1
    if data_start + next_free_cluster - 3 >= sectors - reserved:
        raise ValueError("EFI applications do not fit in FAT image")

    root_start = (1 + fat_sectors) * SECTOR_SIZE
    image[root_start:root_start + 32] = _directory_entry(
        b"IA64 TEST  ", 0x08, 0)
    image[root_start + 32:root_start + 64] = _directory_entry(
        b"EFI        ", 0x10, 2)
    efi_dir_start = data_start * SECTOR_SIZE
    image[efi_dir_start:efi_dir_start + 32] = _directory_entry(
        b".          ", 0x10, 2)
    image[efi_dir_start + 32:efi_dir_start + 64] = _directory_entry(
        b"..         ", 0x10, 0)
    image[efi_dir_start + 64:efi_dir_start + 96] = _directory_entry(
        b"BOOT       ", 0x10, 3)
    boot_dir_start = (data_start + 1) * SECTOR_SIZE
    image[boot_dir_start:boot_dir_start + 32] = _directory_entry(
        b".          ", 0x10, 3)
    image[boot_dir_start + 32:boot_dir_start + 64] = _directory_entry(
        b"..         ", 0x10, 2)
    image[boot_dir_start + 64:boot_dir_start + 96] = _directory_entry(
        b"BOOTIA64EFI", 0x20, first_file_cluster, len(efi_image))
    file_start = (data_start + first_file_cluster - 2) * SECTOR_SIZE
    image[file_start:file_start + len(efi_image)] = efi_image
    for index, (short_name, contents, first_cluster) in \
            enumerate(extra_records):
        entry_start = boot_dir_start + (3 + index) * 32
        image[entry_start:entry_start + 32] = _directory_entry(
            short_name, 0x20, first_cluster, len(contents))
        file_start = (data_start + first_cluster - 2) * SECTOR_SIZE
        image[file_start:file_start + len(contents)] = contents

    test_offset = len(image) - TEST_BLOCKS * SECTOR_SIZE
    if reserve_test_region:
        for index in range(TEST_BLOCKS * SECTOR_SIZE):
            image[test_offset + index] = (index * 19 + 0x31) & 0xFF
    return image, test_offset


def _fat32_volume(efi_image: bytes,
                  reserve_test_region: bool = False) -> tuple[bytearray, int]:
    """Build a standards-conforming FAT32 volume with two FAT copies."""
    sectors = FAT32_DISK_SECTORS
    reserved_sectors = 32
    fat_count = 2
    fat_sectors = FAT32_SECTORS
    data_start = reserved_sectors + fat_count * fat_sectors
    image = bytearray(sectors * SECTOR_SIZE)

    image[0:3] = b"\xeb\x58\x90"
    image[3:11] = b"QEMUIA64"
    struct.pack_into("<H", image, 11, SECTOR_SIZE)
    image[13] = 1
    struct.pack_into("<H", image, 14, reserved_sectors)
    image[16] = fat_count
    struct.pack_into("<H", image, 17, 0)
    struct.pack_into("<H", image, 19, 0)
    image[21] = 0xF8
    struct.pack_into("<H", image, 22, 0)
    struct.pack_into("<H", image, 24, 32)
    struct.pack_into("<H", image, 26, 64)
    struct.pack_into("<I", image, 28, 0)
    struct.pack_into("<I", image, 32, sectors)
    struct.pack_into("<I", image, 36, fat_sectors)
    struct.pack_into("<H", image, 40, 0)
    struct.pack_into("<H", image, 42, 0)
    struct.pack_into("<I", image, 44, 2)
    struct.pack_into("<H", image, 48, 1)
    struct.pack_into("<H", image, 50, 6)
    image[64] = 0x80
    image[66] = 0x29
    image[67:71] = b"IA64"
    image[71:82] = b"IA64 TEST  "
    image[82:90] = b"FAT32   "
    image[510:512] = b"\x55\xaa"

    fsinfo = memoryview(image)[SECTOR_SIZE:2 * SECTOR_SIZE]
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<I", fsinfo, 484, 0x61417272)
    struct.pack_into("<I", fsinfo, 488, 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 492, 5)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)
    image[6 * SECTOR_SIZE:7 * SECTOR_SIZE] = image[0:SECTOR_SIZE]
    image[7 * SECTOR_SIZE:8 * SECTOR_SIZE] = image[SECTOR_SIZE:2 * SECTOR_SIZE]

    fats = [memoryview(image)[
        (reserved_sectors + index * fat_sectors) * SECTOR_SIZE:
        (reserved_sectors + (index + 1) * fat_sectors) * SECTOR_SIZE]
        for index in range(fat_count)]

    def set_entry(cluster: int, value: int) -> None:
        for fat in fats:
            struct.pack_into("<I", fat, cluster * 4, value & 0x0FFFFFFF)

    end_of_chain = 0x0FFFFFFF
    set_entry(0, 0x0FFFFFF8)
    set_entry(1, end_of_chain)
    set_entry(2, end_of_chain)
    set_entry(3, end_of_chain)
    set_entry(4, end_of_chain)

    file_clusters = (len(efi_image) + SECTOR_SIZE - 1) // SECTOR_SIZE
    first_file_cluster = 5
    last_file_cluster = first_file_cluster + file_clusters - 1
    reserved = TEST_BLOCKS if reserve_test_region else 0
    if data_start + last_file_cluster - 2 >= sectors - reserved:
        raise ValueError("EFI application does not fit in FAT32 image")
    for cluster in range(first_file_cluster, last_file_cluster + 1):
        set_entry(cluster, end_of_chain if cluster == last_file_cluster
                  else cluster + 1)

    def cluster_offset(cluster: int) -> int:
        return (data_start + cluster - 2) * SECTOR_SIZE

    root_start = cluster_offset(2)
    image[root_start:root_start + 32] = _directory_entry(
        b"IA64 TEST  ", 0x08, 0)
    image[root_start + 32:root_start + 64] = _directory_entry(
        b"EFI        ", 0x10, 3)
    efi_dir_start = cluster_offset(3)
    image[efi_dir_start:efi_dir_start + 32] = _directory_entry(
        b".          ", 0x10, 3)
    image[efi_dir_start + 32:efi_dir_start + 64] = _directory_entry(
        b"..         ", 0x10, 2)
    image[efi_dir_start + 64:efi_dir_start + 96] = _directory_entry(
        b"BOOT       ", 0x10, 4)
    boot_dir_start = cluster_offset(4)
    image[boot_dir_start:boot_dir_start + 32] = _directory_entry(
        b".          ", 0x10, 4)
    image[boot_dir_start + 32:boot_dir_start + 64] = _directory_entry(
        b"..         ", 0x10, 3)
    image[boot_dir_start + 64:boot_dir_start + 96] = _directory_entry(
        b"BOOTIA64EFI", 0x20, first_file_cluster, len(efi_image))
    file_start = cluster_offset(first_file_cluster)
    image[file_start:file_start + len(efi_image)] = efi_image

    test_offset = len(image) - TEST_BLOCKS * SECTOR_SIZE
    if reserve_test_region:
        for index in range(TEST_BLOCKS * SECTOR_SIZE):
            image[test_offset + index] = (index * 19 + 0x31) & 0xFF
    return image, test_offset


def make_fat_disk(path: str | Path, efi_app: str | Path,
                  layout: str = "whole", *, fat32: bool = False,
                  extra_boot_files: tuple[
                      tuple[bytes, str | Path], ...] = ()) -> MediaInfo:
    """Create a whole-disk FAT16 image, MBR partition, or GPT ESP."""
    path = Path(path)
    efi_image = Path(efi_app).read_bytes()
    extra_contents = tuple(
        (name, Path(source).read_bytes()) for name, source in extra_boot_files)
    if fat32 and extra_contents:
        raise ValueError("extra boot files are not supported for FAT32")
    fat, fat_test_offset = (_fat32_volume(efi_image,
                                         reserve_test_region=True)
                             if fat32 else
                             _fat_volume(efi_image,
                                         reserve_test_region=True,
                                         extra_boot_files=extra_contents))
    disk_sectors = FAT32_DISK_SECTORS if fat32 else DISK_SECTORS
    test_offset = fat_test_offset

    if layout == "gpt":
        total_sectors = GPT_ESP_START + disk_sectors + 33
        raw = bytearray(total_sectors * SECTOR_SIZE)
        raw[446 + 4] = 0xEE
        struct.pack_into("<I", raw, 446 + 8, 1)
        struct.pack_into("<I", raw, 446 + 12, total_sectors - 1)
        raw[510:512] = b"\x55\xaa"

        entries = bytearray(128 * 128)
        entries[0:16] = uuid.UUID(
            "c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le
        entries[16:32] = uuid.UUID(
            "12345678-9abc-def0-1234-56789abcdef0").bytes_le
        struct.pack_into("<QQ", entries, 32, GPT_ESP_START,
                         GPT_ESP_START + disk_sectors - 1)
        name = "EFI system partition".encode("utf-16le")
        entries[56:56 + len(name)] = name
        entries_crc = crc32(entries)

        def gpt_header(current: int, backup: int,
                       entries_lba: int) -> bytearray:
            header = bytearray(SECTOR_SIZE)
            header[0:8] = b"EFI PART"
            struct.pack_into("<I", header, 8, 0x00010000)
            struct.pack_into("<I", header, 12, 92)
            struct.pack_into("<QQQQ", header, 24, current, backup,
                             34, total_sectors - 34)
            header[56:72] = uuid.UUID(
                "0fedcba9-8765-4321-fedc-ba9876543210").bytes_le
            struct.pack_into("<QIII", header, 72, entries_lba,
                             128, 128, entries_crc)
            struct.pack_into("<I", header, 16, crc32(header[:92]))
            return header

        backup_entries_lba = total_sectors - 33
        raw[2 * SECTOR_SIZE:34 * SECTOR_SIZE] = entries
        raw[backup_entries_lba * SECTOR_SIZE:
            (backup_entries_lba + 32) * SECTOR_SIZE] = entries
        raw[SECTOR_SIZE:2 * SECTOR_SIZE] = gpt_header(
            1, total_sectors - 1, 2)
        raw[(total_sectors - 1) * SECTOR_SIZE:] = gpt_header(
            total_sectors - 1, 1, backup_entries_lba)
        struct.pack_into("<I", fat, 28, GPT_ESP_START)
        raw[GPT_ESP_START * SECTOR_SIZE:
            (GPT_ESP_START + disk_sectors) * SECTOR_SIZE] = fat
        test_offset += GPT_ESP_START * SECTOR_SIZE
        image = raw
    elif layout in ("mbr", "mbr-fallback"):
        total_sectors = MBR_ESP_START + disk_sectors
        raw = bytearray(total_sectors * SECTOR_SIZE)
        entry = 446
        if layout == "mbr-fallback":
            raw[entry] = 0x80
            raw[entry + 4] = 0x0B
            struct.pack_into("<I", raw, entry + 8, 1)
            struct.pack_into("<I", raw, entry + 12, MBR_ESP_START - 1)
            entry += 16
            raw[entry] = 0x7F
            raw[entry + 4] = 0xEF
        else:
            raw[entry] = 0x80
            raw[entry + 4] = 0x0E
        struct.pack_into("<I", raw, entry + 8, MBR_ESP_START)
        struct.pack_into("<I", raw, entry + 12, disk_sectors)
        raw[510:512] = b"\x55\xaa"
        struct.pack_into("<I", fat, 28, MBR_ESP_START)
        raw[MBR_ESP_START * SECTOR_SIZE:] = fat
        test_offset += MBR_ESP_START * SECTOR_SIZE
        image = raw
    elif layout == "whole":
        image = fat
    else:
        raise ValueError(f"unknown FAT disk layout: {layout}")

    path.write_bytes(image)
    test_data = bytes(image[test_offset:test_offset +
                            TEST_BLOCKS * SECTOR_SIZE])
    return MediaInfo(path, file_sha256(path), test_offset, test_data)


def _write_el_torito(image: bytearray, catalog_lba: int, boot_lba: int,
                     platform_id: int, boot_image: bytes | bytearray) -> None:
    """Write an El Torito catalog and its exact-size no-emulation image."""
    if len(boot_image) % SECTOR_SIZE:
        raise ValueError("El Torito boot image must use whole 512-byte sectors")
    sector_count = len(boot_image) // SECTOR_SIZE
    if not 0 < sector_count <= 0xFFFF:
        raise ValueError("El Torito boot image sector count is out of range")
    start = boot_lba * ISO_SECTOR_SIZE
    if start + len(boot_image) > len(image):
        raise ValueError("El Torito boot image does not fit in ISO image")

    boot_record = _iso_sector(image, 17)
    boot_record[0] = 0
    boot_record[1:6] = b"CD001"
    boot_record[6] = 1
    boot_record[7:30] = b"EL TORITO SPECIFICATION"
    struct.pack_into("<I", boot_record, 71, catalog_lba)
    terminator = _iso_sector(image, 18)
    terminator[0] = 0xFF
    terminator[1:6] = b"CD001"
    terminator[6] = 1

    catalog = _iso_sector(image, catalog_lba)
    catalog[0] = 1
    catalog[1] = platform_id
    catalog[0x1E] = 0x55
    catalog[0x1F] = 0xAA
    checksum = (-sum(struct.unpack_from("<16H", catalog, 0))) & 0xFFFF
    struct.pack_into("<H", catalog, 0x1C, checksum)
    catalog[0x20] = 0x88
    struct.pack_into("<H", catalog, 0x26, sector_count)
    struct.pack_into("<I", catalog, 0x28, boot_lba)
    image[start:start + len(boot_image)] = boot_image


def make_el_torito_iso(path: str | Path, efi_app: str | Path,
                        platform_id: int = 0xEF) -> MediaInfo:
    """Create an ISO9660 image with a no-emulation FAT boot image."""
    path = Path(path)
    fat, _ = _fat_volume(Path(efi_app).read_bytes(), sectors=256,
                         fat_sectors=4, root_entries=64)
    iso_sectors = 512
    catalog_lba = 19
    boot_lba = 64
    image = bytearray(iso_sectors * ISO_SECTOR_SIZE)
    _write_iso9660(image, iso_sectors, 20, 22)
    _write_el_torito(image, catalog_lba, boot_lba, platform_id, fat)
    path.write_bytes(image)
    return MediaInfo(path, file_sha256(path))


# Minimal ECMA-167/UDF 2.01 bridge layout used by the firmware parser.
UDF_TOTAL_SECTORS = 544
UDF_CATALOG_LBA = 23
UDF_BOOT_LBA = 32
UDF_MAIN_VDS_LBA = 257
UDF_INTEGRITY_LBA = 280
UDF_RESERVE_VDS_LBA = 536
UDF_PARTITION_START = 304
UDF_PARTITION_LENGTH = 128
UDF_PARTITION_NUMBER = 2989
UDF_PARTITION_REFERENCE = 0
UDF_ROOT_ICB = 2
UDF_EFI_ICB = 3
UDF_BOOT_ICB = 4
UDF_APP_ICB = 5
UDF_APP_DATA = 8


def _iso_sector(image: bytearray, lba: int) -> memoryview:
    start = lba * ISO_SECTOR_SIZE
    return memoryview(image)[start:start + ISO_SECTOR_SIZE]


def _finish_udf_tag(buffer: memoryview | bytearray, tag_id: int,
                    location: int, crc_length: int) -> None:
    struct.pack_into("<H", buffer, 0, tag_id)
    struct.pack_into("<H", buffer, 2, 3)
    buffer[4] = 0
    buffer[5] = 0
    struct.pack_into("<H", buffer, 6, 1)
    struct.pack_into("<H", buffer, 8,
                     crc16_ccitt(buffer[16:16 + crc_length]))
    struct.pack_into("<H", buffer, 10, crc_length)
    struct.pack_into("<I", buffer, 12, location)
    buffer[4] = sum(buffer[index] for index in range(16)
                    if index != 4) & 0xFF


def _write_regid(buffer: memoryview | bytearray, offset: int, identifier: str,
                 udf_revision: int | None = None) -> None:
    encoded = identifier.encode("ascii")
    if len(encoded) > 23:
        raise ValueError("ECMA-167 regid identifier is too long")
    buffer[offset] = 0
    buffer[offset + 1:offset + 1 + len(encoded)] = encoded
    if udf_revision is not None:
        struct.pack_into("<H", buffer, offset + 24, udf_revision)


def _write_charspec(buffer: memoryview, offset: int) -> None:
    identifier = b"OSTA Compressed Unicode"
    buffer[offset] = 0
    buffer[offset + 1:offset + 1 + len(identifier)] = identifier


def _write_dstring(buffer: memoryview, offset: int, length: int,
                   value: str) -> None:
    encoded = b"\x08" + value.encode("ascii")
    if len(encoded) >= length:
        raise ValueError("ECMA-167 dstring value is too long")
    buffer[offset:offset + length] = bytes(length)
    buffer[offset:offset + len(encoded)] = encoded
    buffer[offset + length - 1] = len(encoded)


def _udf_timestamp() -> bytes:
    # Type 1 (local time), UTC offset zero, 2024-01-01 00:00:00.000000.
    return struct.pack("<HH8B", 0x1000, 2024, 1, 1, 0, 0, 0, 0, 0, 0)


def _write_long_ad(buffer: memoryview | bytearray, offset: int, length: int,
                   location: int, partition_reference: int) -> None:
    struct.pack_into("<I", buffer, offset, length)
    struct.pack_into("<I", buffer, offset + 4, location)
    struct.pack_into("<H", buffer, offset + 8, partition_reference)


def _udf_fid(name: str, characteristics: int, icb: int,
             parent_icb: int) -> bytes:
    name_bytes = b"" if not name else bytes([8]) + name.encode("ascii")
    total = (38 + len(name_bytes) + 3) & ~3
    fid = bytearray(total)
    struct.pack_into("<H", fid, 16, 1)
    fid[18] = characteristics
    fid[19] = len(name_bytes)
    _write_long_ad(fid, 20, ISO_SECTOR_SIZE, icb,
                   UDF_PARTITION_REFERENCE)
    struct.pack_into("<I", fid, 30, parent_icb)
    fid[38:38 + len(name_bytes)] = name_bytes
    _finish_udf_tag(fid, 257, parent_icb, total - 16)
    return bytes(fid)


def _write_udf_file_entry_header(descriptor: memoryview, icb: int,
                                 parent_icb: int, file_type: int,
                                 allocation_type: int, information_length: int,
                                 logical_blocks: int) -> None:
    # ECMA-167 4/14.6 ICB tag.
    struct.pack_into("<H", descriptor, 20, 4)  # strategy type
    struct.pack_into("<H", descriptor, 24, 1)  # maximum entries
    descriptor[27] = file_type
    struct.pack_into("<I", descriptor, 28, parent_icb)
    struct.pack_into("<H", descriptor, 32, UDF_PARTITION_REFERENCE)
    struct.pack_into("<H", descriptor, 34, allocation_type)

    # ECMA-167 4/14.9 File Entry.
    struct.pack_into("<II", descriptor, 36, 0xFFFFFFFF, 0xFFFFFFFF)
    struct.pack_into("<I", descriptor, 44, 0x000014A5)
    struct.pack_into("<H", descriptor, 48, 1)
    struct.pack_into("<Q", descriptor, 56, information_length)
    struct.pack_into("<Q", descriptor, 64, logical_blocks)
    timestamp = _udf_timestamp()
    descriptor[72:84] = timestamp
    descriptor[84:96] = timestamp
    descriptor[96:108] = timestamp
    struct.pack_into("<I", descriptor, 108, 1)
    _write_regid(descriptor, 128, "*QEMU IA64")
    struct.pack_into("<Q", descriptor, 160, icb + 1)
    struct.pack_into("<I", descriptor, 168, 0)


def _write_udf_directory(image: bytearray, icb: int, parent_icb: int,
                         data: bytes) -> None:
    descriptor = _iso_sector(image, UDF_PARTITION_START + icb)
    _write_udf_file_entry_header(descriptor, icb, parent_icb, 4, 3,
                                 len(data), 0)
    struct.pack_into("<I", descriptor, 172, len(data))
    descriptor[176:176 + len(data)] = data
    _finish_udf_tag(descriptor, 261, icb, 160 + len(data))


def _write_udf_file(image: bytearray, icb: int, parent_icb: int,
                    data_block: int, data: bytes) -> None:
    blocks = (len(data) + ISO_SECTOR_SIZE - 1) // ISO_SECTOR_SIZE
    if data_block + blocks > UDF_PARTITION_LENGTH:
        raise ValueError("EFI application does not fit in UDF partition")
    descriptor = _iso_sector(image, UDF_PARTITION_START + icb)
    _write_udf_file_entry_header(descriptor, icb, parent_icb, 5, 0,
                                 len(data), blocks)
    struct.pack_into("<I", descriptor, 172, 8)
    struct.pack_into("<II", descriptor, 176, len(data), data_block)
    _finish_udf_tag(descriptor, 261, icb, 168)
    start = (UDF_PARTITION_START + data_block) * ISO_SECTOR_SIZE
    image[start:start + len(data)] = data


def make_udf_bridge_iso(path: str | Path, efi_app: str | Path, *,
                        extra_boot_files: tuple[
                            tuple[bytes, str | Path], ...] = ()) -> MediaInfo:
    """Create a UDF bridge with native and El Torito EFI boot paths."""
    path = Path(path)
    image = bytearray(UDF_TOTAL_SECTORS * ISO_SECTOR_SIZE)
    efi_image = Path(efi_app).read_bytes()
    extra_contents = tuple(
        (name, Path(source).read_bytes()) for name, source in extra_boot_files)
    fat, _ = _fat_volume(efi_image, sectors=256,
                         fat_sectors=4, root_entries=64,
                         extra_boot_files=extra_contents)
    files = [("BOOTIA64.EFI", efi_image)]
    for name, contents in extra_contents:
        stem = name[:8].decode("ascii").rstrip()
        extension = name[8:].decode("ascii").rstrip()
        files.append((stem + ("." + extension if extension else ""), contents))
    if UDF_APP_ICB + len(files) > UDF_APP_DATA:
        raise ValueError("Too many EFI applications for UDF file entries")

    _write_iso9660(image, UDF_TOTAL_SECTORS, 24, 26)
    _write_el_torito(image, UDF_CATALOG_LBA, UDF_BOOT_LBA, 0xEF, fat)

    for lba, identifier in ((19, b"BEA01"), (20, b"NSR03"),
                            (21, b"TEA01")):
        descriptor = _iso_sector(image, lba)
        descriptor[1:6] = identifier
        descriptor[6] = 1
    main_vds_blocks = 6
    for lba in (256, UDF_TOTAL_SECTORS - 257,
                UDF_TOTAL_SECTORS - 1):
        descriptor = _iso_sector(image, lba)
        struct.pack_into("<I", descriptor, 16,
                         main_vds_blocks * ISO_SECTOR_SIZE)
        struct.pack_into("<I", descriptor, 20, UDF_MAIN_VDS_LBA)
        struct.pack_into("<I", descriptor, 24,
                         main_vds_blocks * ISO_SECTOR_SIZE)
        struct.pack_into("<I", descriptor, 28, UDF_RESERVE_VDS_LBA)
        _finish_udf_tag(descriptor, 2, lba, 496)

    primary = _iso_sector(image, UDF_MAIN_VDS_LBA)
    struct.pack_into("<I", primary, 16, 1)
    struct.pack_into("<I", primary, 20, 0)
    _write_dstring(primary, 24, 32, "IA64_TEST")
    struct.pack_into("<HHHH", primary, 56, 1, 1, 2, 3)
    struct.pack_into("<II", primary, 64, 1, 1)
    _write_dstring(primary, 72, 128, "IA64_TEST_SET")
    _write_charspec(primary, 200)
    _write_charspec(primary, 264)
    _write_regid(primary, 344, "*QEMU IA64")
    primary[376:388] = _udf_timestamp()
    _write_regid(primary, 388, "*QEMU IA64")
    _finish_udf_tag(primary, 1, UDF_MAIN_VDS_LBA, 496)

    implementation_lba = UDF_MAIN_VDS_LBA + 1
    implementation = _iso_sector(image, implementation_lba)
    struct.pack_into("<I", implementation, 16, 2)
    _write_regid(implementation, 20, "*UDF LV Info", 0x0201)
    _write_charspec(implementation, 52)
    _write_dstring(implementation, 116, 128, "IA64_TEST")
    _write_regid(implementation, 352, "*QEMU IA64", 0x0201)
    _finish_udf_tag(implementation, 4, implementation_lba, 496)

    partition_lba = UDF_MAIN_VDS_LBA + 2
    partition = _iso_sector(image, partition_lba)
    struct.pack_into("<I", partition, 16, 3)
    struct.pack_into("<H", partition, 20, 1)
    struct.pack_into("<H", partition, 22, UDF_PARTITION_NUMBER)
    _write_regid(partition, 24, "+NSR03", 0x0201)
    struct.pack_into("<I", partition, 184, 1)
    struct.pack_into("<I", partition, 188, UDF_PARTITION_START)
    struct.pack_into("<I", partition, 192, UDF_PARTITION_LENGTH)
    _write_regid(partition, 196, "*QEMU IA64", 0x0201)
    _finish_udf_tag(partition, 5, partition_lba, 496)

    logical_lba = UDF_MAIN_VDS_LBA + 3
    logical = _iso_sector(image, logical_lba)
    struct.pack_into("<I", logical, 16, 4)
    _write_charspec(logical, 20)
    _write_dstring(logical, 84, 128, "IA64_TEST")
    struct.pack_into("<I", logical, 212, ISO_SECTOR_SIZE)
    _write_regid(logical, 216, "*OSTA UDF Compliant", 0x0201)
    _write_long_ad(logical, 248, ISO_SECTOR_SIZE, 0,
                   UDF_PARTITION_REFERENCE)
    struct.pack_into("<I", logical, 264, 6)
    struct.pack_into("<I", logical, 268, 1)
    _write_regid(logical, 272, "*QEMU IA64", 0x0201)
    struct.pack_into("<II", logical, 432, ISO_SECTOR_SIZE,
                     UDF_INTEGRITY_LBA)
    logical[440] = 1
    logical[441] = 6
    struct.pack_into("<H", logical, 442, 1)
    struct.pack_into("<H", logical, 444, UDF_PARTITION_NUMBER)
    _finish_udf_tag(logical, 6, logical_lba, 430)

    unallocated_lba = UDF_MAIN_VDS_LBA + 4
    unallocated = _iso_sector(image, unallocated_lba)
    struct.pack_into("<I", unallocated, 16, 5)
    struct.pack_into("<I", unallocated, 20, 0)
    _finish_udf_tag(unallocated, 7, unallocated_lba, 496)

    terminating_lba = UDF_MAIN_VDS_LBA + 5
    terminating = _iso_sector(image, terminating_lba)
    _finish_udf_tag(terminating, 8, terminating_lba, 496)

    vds_tags = (1, 4, 5, 6, 7, 8)
    for index, tag in enumerate(vds_tags):
        source = _iso_sector(image, UDF_MAIN_VDS_LBA + index)
        reserve = _iso_sector(image, UDF_RESERVE_VDS_LBA + index)
        reserve[:] = source
        _finish_udf_tag(reserve, tag, UDF_RESERVE_VDS_LBA + index, 496)

    integrity = _iso_sector(image, UDF_INTEGRITY_LBA)
    integrity[16:28] = _udf_timestamp()
    struct.pack_into("<I", integrity, 28, 1)
    struct.pack_into("<I", integrity, 72, 1)
    struct.pack_into("<I", integrity, 76, 46)
    struct.pack_into("<II", integrity, 80, 0, UDF_PARTITION_LENGTH)
    _write_regid(integrity, 88, "*QEMU IA64", 0x0201)
    struct.pack_into("<IIHHH", integrity, 120, len(files), 3,
                     0x0201, 0x0201, 0x0201)
    _finish_udf_tag(integrity, 9, UDF_INTEGRITY_LBA, 118)
    file_set = _iso_sector(image, UDF_PARTITION_START)
    file_set[16:28] = _udf_timestamp()
    struct.pack_into("<HHII", file_set, 28, 3, 3, 1, 1)
    _write_charspec(file_set, 48)
    _write_dstring(file_set, 112, 128, "IA64_TEST")
    _write_charspec(file_set, 240)
    _write_dstring(file_set, 304, 32, "IA64_TEST_FILES")
    _write_long_ad(file_set, 400, ISO_SECTOR_SIZE, UDF_ROOT_ICB,
                   UDF_PARTITION_REFERENCE)
    _write_regid(file_set, 416, "*OSTA UDF Compliant", 0x0201)
    _finish_udf_tag(file_set, 256, 0, 496)

    directory = 0x02
    parent = 0x08
    root_data = b"".join((
        _udf_fid("", directory | parent, UDF_ROOT_ICB, UDF_ROOT_ICB),
        _udf_fid("EFI", directory, UDF_EFI_ICB, UDF_ROOT_ICB),
    ))
    efi_data = b"".join((
        _udf_fid("", directory | parent, UDF_ROOT_ICB, UDF_EFI_ICB),
        _udf_fid("BOOT", directory, UDF_BOOT_ICB, UDF_EFI_ICB),
    ))
    boot_data = _udf_fid("", directory | parent, UDF_EFI_ICB, UDF_BOOT_ICB)
    boot_data += b"".join(
        _udf_fid(name, 0, UDF_APP_ICB + index, UDF_BOOT_ICB)
        for index, (name, _) in enumerate(files))
    _write_udf_directory(image, UDF_ROOT_ICB, UDF_ROOT_ICB, root_data)
    _write_udf_directory(image, UDF_EFI_ICB, UDF_ROOT_ICB, efi_data)
    _write_udf_directory(image, UDF_BOOT_ICB, UDF_EFI_ICB, boot_data)
    data_block = UDF_APP_DATA
    for index, (_, contents) in enumerate(files):
        _write_udf_file(image, UDF_APP_ICB + index, UDF_BOOT_ICB,
                        data_block, contents)
        data_block += (len(contents) + ISO_SECTOR_SIZE - 1) // ISO_SECTOR_SIZE

    path.write_bytes(image)
    return MediaInfo(path, file_sha256(path))
