"""Decode the static AML resources emitted by the IA-64 firmware."""

# SPDX-License-Identifier: GPL-2.0-or-later

from collections import Counter
import struct


def package(aml, offset):
    lead = aml[offset]
    following = lead >> 6
    length = lead & (0x0F if following else 0x3F)
    for index in range(following):
        length |= aml[offset + index + 1] << (4 + 8 * index)
    start, end = offset + following + 1, offset + length
    if not start <= end <= len(aml):
        raise AssertionError("invalid AML package bounds")
    return start, end


def integer(aml, offset):
    opcode = aml[offset]
    if opcode in (0, 1):
        return opcode, offset + 1
    width = {0x0A: 1, 0x0B: 2, 0x0C: 4, 0x0E: 8}[opcode]
    return int.from_bytes(aml[offset + 1:offset + 1 + width], "little"), \
        offset + 1 + width


def device(aml, name):
    offset = 0
    while True:
        offset = aml.find(b"\x5b\x82", offset)
        if offset < 0:
            raise AssertionError(f"missing AML device {name!r}")
        start, end = package(aml, offset + 2)
        if aml[start:start + 4] == name:
            return aml[start + 4:end]
        offset += 2


def name_value(aml, name):
    offset = aml.find(b"\x08" + name)
    if offset < 0:
        raise AssertionError(f"missing AML name {name!r}")
    return offset + 5


def resources(aml):
    offset = name_value(aml, b"_CRS")
    if aml[offset] != 0x11:
        raise AssertionError("_CRS is not a static buffer")
    start, end = package(aml, offset + 1)
    size, start = integer(aml, start)
    if size != end - start:
        raise AssertionError("incorrect resource buffer size")
    result = []
    while start < end:
        tag = aml[start]
        size = (3 + struct.unpack_from("<H", aml, start + 1)[0]
                if tag & 0x80 else 1 + (tag & 7))
        if start + size > end:
            raise AssertionError("truncated resource descriptor")
        result.append(aml[start:start + size])
        start += size
    if result.pop() != b"\x79\x00":
        raise AssertionError("missing resource EndTag")
    return result


def address_spaces(aml):
    result = []
    for data in resources(aml):
        width = {0x88: 2, 0x87: 4, 0x8A: 8}.get(data[0])
        if width is None:
            continue
        if len(data) != 6 + 5 * width:
            raise AssertionError("unexpected address descriptor length")
        fields = struct.unpack_from("<" + {2: "H", 4: "I", 8: "Q"}[width] * 5,
                                    data, 6)
        if fields[0] != 0:
            raise AssertionError("unexpected address granularity")
        result.append((width, *data[3:6], *fields[1:]))
    return result


def io_window(base, size):
    return (2, 1, 0x0C, 3, base, base + size - 1, 0, size)


def memory_window(base, size, width=4, translation=0):
    return (width, 0, 0x0C, 1, base, base + size - 1, translation, size)


def assert_pci_windows(test, aml, roots):
    upstream = []
    for index, expected in enumerate(roots):
        child = address_spaces(device(aml, f"PCI{index}".encode()))
        test.assertEqual([entry for entry in child if entry[1] != 2], expected)
        for (width, kind, flags, attributes, lo, hi, translation,
             size) in expected:
            if kind == 0:
                lo += translation
                hi += translation
                translation = 0
            upstream.append((width, kind, flags, attributes, lo, hi,
                             translation, size))
    test.assertEqual(Counter(address_spaces(device(aml, b"SBA0"))),
                     Counter(upstream))


def assert_zx2000_uarts(test, aml):
    test.assertEqual(aml.count(b"\x08_HID\x0c\x41\xd0\x05\x01"), 2,
                     "SSDT must contain only the two platform UARTs")
    parent = b"\x5c\x2f\x03_SB_SBA0PCI2"
    scope = None
    for offset, opcode in enumerate(aml):
        if opcode == 0x10:
            try:
                start, end = package(aml, offset + 1)
            except (AssertionError, IndexError):
                continue
            if aml[start:start + len(parent) + 2] == parent + b"\x5b\x82":
                body, _ = package(aml, start + len(parent) + 2)
                if aml[body:body + 4] == b"UAR0":
                    scope = aml[start + len(parent):end]
                    break
    test.assertIsNotNone(scope)
    for uid, base, gsi in ((0, 0xFF5E0000, 45), (1, 0xFF5E2000, 46)):
        uart = device(scope, f"UAR{uid}".encode())
        test.assertEqual(integer(uart, name_value(uart, b"_HID"))[0],
                         0x0105D041)
        test.assertEqual(integer(uart, name_value(uart, b"_UID"))[0], uid)
        test.assertEqual(integer(uart, name_value(uart, b"_STA"))[0], 15)
        test.assertEqual(address_spaces(uart),
                         [(4, 0, 0x0D, 1, base, base + 7, 0, 8)])
        descriptors = resources(uart)
        test.assertEqual(len(descriptors), 2)
        test.assertEqual(descriptors[1], b"\x89\x06\x00\x01\x01" +
                         struct.pack("<I", gsi))
