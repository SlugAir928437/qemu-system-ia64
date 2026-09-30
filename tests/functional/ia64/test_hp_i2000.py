#!/usr/bin/env python3
"""Firmware boot tests for the HP i2000 machine."""

# SPDX-License-Identifier: GPL-2.0-or-later

import os
import re
from pathlib import Path
from subprocess import run

from qemu_test import QemuSystemTest, wait_for_console_pattern

from ia64.efi_build import app_path
from ia64.media import (make_el_torito_iso, make_fat_disk,
                        make_udf_bridge_iso, UDF_BOOT_LBA)
from ia64.protocol import wait_for_suite


SMOKE_CASES = {
    "entry", "system-table", "loaded-image", "device-path",
    "root-device-path", "console-output", "console-variables",
    "low-memory-allocation",
}

GRAPHICS_CASES = {
    "protocols", "protocol-list", "pci-location", "pci-dma",
    "pci-attributes", "pci-bars", "device-path", "gop", "vbe-mode", "uga",
    "framebuffer-io", "memory-map",
}

INPUT_CASES = {
    "text-input-ex", "ready-basic", "read-key-stroke",
    "ready-modifier", "modifier-key", "modifier-state", "ready-extended",
    "extended-scan-code",
}

LOADER_CASES = {
    "image-placement", "low-memory-map", "runtime-map",
    "firmware-aperture", "sal-entrypoint", "sal-memory-descriptors",
    "sal-call", "sal-chipset-config", "direct-alias", "automatic-allocation",
    "address-allocation", "acpi-topology", "acpi-console",
}

PCI_ROOT_CASES = {
    "system-table", "protocols", "device-paths", "config-access",
    "host-enumeration",
}

RUNTIME_CASES = {"boot-variables", "get-time", "set-time", "fadt-reset"}

SMP_MERCED_CASES = {
    "sal-ap-wake", "merced-rendezvous", "merced-rendezvous-return",
    "zero-alat-check-reload",
}


class HPI2000Boot(QemuSystemTest):
    @staticmethod
    def send_keys(vm, qcodes):
        vm.cmd("send-key", keys=[
            {"type": "qcode", "data": qcode} for qcode in qcodes
        ], hold_time=50)

    def media_path(self, name: str) -> Path:
        configured = os.environ.get("IA64_TEST_MEDIA_DIR")
        if not configured:
            return Path(self.scratch_file(name))

        directory = Path(configured)
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / f"hp-i2000-{os.getpid()}-{name}"
        self.addCleanup(path.unlink, missing_ok=True)
        return path

    def test_firmware_ready(self):
        self.require_accelerator("tcg")
        vm = self.get_vm()
        vm.set_machine("hp-i2000,nvram=none")
        vm.set_console()
        vm.add_args(
            "-accel", "tcg",
            "-m", "2G",
            "-smp", "1",
            "-display", "none",
            "-vga", "quadro2",
            "-net", "none",
        )
        vm.launch()

        output = wait_for_console_pattern(
            self, "Firmware ready.",
            failure_message="Invalid IA-64 platform descriptor", vm=vm,
        )
        self.assertIn(b"PCI Root Bridge I/O:  published", output)
        self.assertIn(b"PCI Host Bridge:       published", output)
        self.assertIn(b"ACPI MCFG (PCIe):     suppressed", output)
        self.assertIn(b"ACPI SSDT (CPU/UART/PS2): published", output)
        self.assertIn(b"SCSI controller:      ISP12160 polling", output)
        self.assertIn(b" bmdma=0x", output)
        self.assertIn(b"Console In:           Serial/PS2/USB ready", output)
        self.assertIn(b"NVRAM Variables:      enabled", output)
        self.assertIn(b"EFI Time Services:    enabled", output)
        self.assertIn(
            b"ResetSystem:          enabled (shutdown unavailable)", output
        )
        self.assertIn(b"Firmware flags:       0x000000000000001B", output)
        self.assertIn(b"GOP/UGA VGA text console ready", output)
        self.assertIn(b"Graphics Output:      GOP/UGA VGA BGRx", output)
        self.assertTrue(vm.is_running(), "QEMU exited during firmware boot")

    def test_scsi_disk_boot(self):
        self.run_scsi_disk_boot("none")

    def test_scsi_disk_boot_with_graphics(self):
        self.run_scsi_disk_boot("quadro2")

    def test_edd30_paths_and_persistence(self):
        self.require_accelerator("tcg")
        disk = self.media_path("edd30.img")
        nvram = self.media_path("edd30.nvram")
        make_fat_disk(disk, app_path("smoke"), layout="gpt")
        nvram.write_bytes(bytes(64 * 1024))

        def command(vm, text, expected):
            vm.console_socket.sendall((text + "\r").encode("ascii"))
            return wait_for_console_pattern(self, expected, vm=vm)

        for index, (mode, next_mode) in enumerate(
                (("on", "off"), ("off", "on"), ("on", None))):
            vm = self.get_vm(name=f"edd30-{index}")
            vm.set_machine(f"hp-i2000,nvram={nvram}")
            vm.set_console()
            vm.add_args("-accel", "tcg", "-m", "2G", "-smp", "1",
                        "-display", "none", "-net", "none",
                        "-drive", f"file={disk},format=raw")
            vm.launch()
            wait_for_console_pattern(self, "Press F2, F12, or Delete", vm=vm)
            self.send_keys(vm, ["f2"])
            wait_for_console_pattern(self, "IA-64 EFI shell", vm=vm)
            command(vm, "edd30", f"EDD30: {mode}")
            command(vm, "edd30 invalid", "Usage: edd30 [on|off]")
            command(vm, "edd30", f"EDD30: {mode}")
            vm.console_socket.sendall(b"run fs0:\\EFI\\BOOT\\BOOTIA64.EFI\r")
            result = wait_for_suite(
                vm.console_socket, "smoke", SMOKE_CASES, 60.0,
                process_alive=vm.is_running)
            self.assertEqual(result.failed, 0)
            paths = [line.split("EFI boot device path: ", 1)[1].strip()
                     for line in result.raw_console.splitlines()
                     if "EFI boot device path: " in line]
            self.assertEqual(len(paths), 1)
            path = bytes.fromhex(paths[0])
            offset = 0
            nodes = []
            while offset < len(path):
                size = int.from_bytes(path[offset + 2:offset + 4], "little")
                self.assertGreaterEqual(size, 4)
                nodes.append(path[offset:offset + size])
                offset += size
            self.assertEqual(nodes[0][:2], b"\x02\x01")
            self.assertEqual(nodes[1][:2], b"\x01\x01")
            if mode == "off":
                self.assertEqual(nodes[2], bytes.fromhex(
                    "01041800c5fa31cf4ec2d21185f300a0c93ec93b80000000"))
            else:
                self.assertEqual(nodes[2], bytes.fromhex("0302080000000000"))
            self.assertEqual(nodes[3][:2], b"\x04\x01")
            self.assertEqual(nodes[-1], b"\x7f\xff\x04\x00")
            if next_mode is not None:
                command(vm, f"edd30 {next_mode}",
                        "EDD30 saved; reset to apply.")
                command(vm, "edd30", f"EDD30: {next_mode}")
            vm.shutdown()

    @staticmethod
    def device_path_nodes(output):
        paths = re.findall(r"EFI boot device path: ([0-9A-Fa-f]+)", output)
        if len(paths) != 1:
            raise AssertionError(f"Expected one boot path, got {paths}")
        path = bytes.fromhex(paths[0])
        nodes = []
        while path:
            size = int.from_bytes(path[2:4], "little")
            if size < 4 or size > len(path):
                raise AssertionError("Malformed device path")
            nodes.append(path[:size])
            path = path[size:]
        return nodes

    def test_multiple_storage_devices(self):
        self.require_accelerator("tcg")
        nvram = self.media_path("multiple-storage.nvram")
        nvram.write_bytes(bytes(64 * 1024))
        devices = [
            ("scsi", channel, target, 0,
             "cd" if target in (2, 14) else "disk")
            for channel in range(2) for target in range(16) if target != 7
        ]
        devices += [("scsi", 0, 0, 31, "disk"),
                    ("scsi", 1, 15, 1, "cd")]
        devices += [("ide", channel, unit, 0,
                     "cd" if channel == unit else "disk")
                    for channel in range(2) for unit in range(2)]
        drive_args = []
        expected = {}
        for index, device in enumerate(devices):
            kind, channel, target, lun, medium = device
            path = self.media_path(f"multiple-{index}.img")
            label = f"F{index:02}"
            optical = medium == "cd"
            udf = device == ("scsi", 1, 2, 0, "cd")
            if optical:
                if udf:
                    make_udf_bridge_iso(
                        path, app_path("smoke"),
                        extra_boot_files=((b"START   EFI",
                                           app_path("start-image-child")),))
                else:
                    make_el_torito_iso(path, app_path("smoke"))
                fat_offset = (UDF_BOOT_LBA if udf else 64) * 2048
                raw_label = "IA64_TEST" if udf else f"O{index:02}"
                expected[raw_label] = (device, udf)
            else:
                layout = ("whole", "mbr", "gpt")[index % 3]
                make_fat_disk(path, app_path("smoke"), layout=layout)
                fat_offset = 0 if layout == "whole" else 63 * 512
            with path.open("r+b") as image:
                image.seek(fat_offset + 43)
                image.write(label.encode("ascii").ljust(11, b" "))
                if optical and not udf:
                    image.seek(16 * 2048 + 40)
                    image.write(raw_label.encode("ascii").ljust(32, b" "))
            expected[label] = (device, True)
            drive_args += ["-drive", f"if=none,id=media{index},file={path},"
                           f"format=raw,readonly={'on' if optical else 'off'}"]
            if kind == "scsi":
                drive_args += ["-device",
                               f"scsi-{'cd' if optical else 'hd'},"
                               f"bus=isp12160-scsi.0,channel={channel},"
                               f"scsi-id={target},lun={lun},drive=media{index}"]
            else:
                drive_args += ["-device",
                               f"ide-{'cd' if optical else 'hd'},"
                               f"bus=ide.{channel},unit={target},"
                               f"drive=media{index}"]
        scsi_devices = sorted((d for d in devices if d[0] == "scsi"),
                              key=lambda d: (d[1], d[3], d[2]))
        edd_numbers = {}
        for medium, base in (("disk", 0x80), ("cd", 0xe0)):
            matching = [d for d in scsi_devices if d[4] == medium]
            edd_numbers.update((d, base + i) for i, d in enumerate(matching))

        for mode in ("on", "off"):
            vm = self.get_vm(name=f"multiple-storage-{mode}")
            vm.set_machine(f"hp-i2000,nvram={nvram}")
            vm.set_console()
            vm.add_args("-accel", "tcg", "-m", "2G", "-smp", "1",
                        "-display", "none", "-net", "none", *drive_args)
            vm.launch()
            initial = wait_for_console_pattern(
                self, "Press F2, F12, or Delete", vm=vm)
            self.assertNotIn(b"publication failed", initial)
            self.send_keys(vm, ["f2"])
            wait_for_console_pattern(self, "IA-64 EFI shell", vm=vm)
            vm.console_socket.sendall(b"map\rinfo\r")
            output = wait_for_console_pattern(
                self, "NVRAM backing:", vm=vm).decode("ascii")
            mappings = re.findall(r"fs(\d+):\*?\s+(\S+)\s+\d+ MiB", output)
            # The shell prints an initial map before processing commands.
            mapped = {label: int(number) for number, label in mappings}
            self.assertEqual(set(mapped), set(expected), output)
            self.assertGreater(max(mapped.values()), 16)
            paths = set()
            for label, fs_index in mapped.items():
                device, bootable = expected[label]
                kind, channel, target, lun, _ = device
                if not bootable:
                    continue
                vm.console_socket.sendall(
                    f"ls fs{fs_index}:\\EFI\\BOOT\r".encode("ascii"))
                wait_for_console_pattern(self, "BOOTIA64.EFI", vm=vm)
                vm.console_socket.sendall(
                    f"boot fs{fs_index}:\r".encode("ascii"))
                required = SMOKE_CASES
                if label == "IA64_TEST":
                    required = required | {"optical-whole-media-path",
                                           "optical-whole-media-load"}
                result = wait_for_suite(
                    vm.console_socket, "smoke", required, 60.0,
                    process_alive=vm.is_running)
                self.assertEqual(result.failed, 0, result.raw_console)
                nodes = self.device_path_nodes(result.raw_console)
                encoded = b"".join(nodes)
                self.assertNotIn(encoded, paths)
                paths.add(encoded)
                offset = 2
                if kind == "scsi" and channel:
                    self.assertEqual(nodes[offset],
                                     bytes.fromhex("0105080001000000"))
                    offset += 1
                if kind == "ide":
                    self.assertEqual(nodes[offset],
                                     bytes([3, 1, 8, 0, channel, target, 0, 0]))
                elif mode == "on":
                    self.assertEqual(nodes[offset],
                                     bytes([3, 2, 8, 0, target, 0, lun, 0]))
                else:
                    self.assertEqual(nodes[offset], bytes.fromhex(
                        "01041800c5fa31cf4ec2d21185f300a0c93ec93b") +
                        edd_numbers[device].to_bytes(4, "little"))
            if mode == "on":
                vm.console_socket.sendall(b"edd30 off\r")
                wait_for_console_pattern(
                    self, "EDD30 saved; reset to apply.", vm=vm)
            vm.shutdown()

    def test_scsi_all_luns(self):
        self.require_accelerator("tcg")
        path = self.media_path("all-luns.img")
        nvram = self.media_path("all-luns.nvram")
        make_fat_disk(path, app_path("smoke"), layout="gpt")
        nvram.write_bytes(bytes(64 * 1024))
        setup = self.get_vm(name="all-luns-setup")
        setup.set_machine(f"hp-i2000,nvram={nvram}")
        setup.set_console()
        setup.add_args("-accel", "tcg", "-m", "2G", "-display", "none",
                       "-net", "none")
        setup.launch()
        wait_for_console_pattern(self, "Press F2, F12, or Delete", vm=setup)
        self.send_keys(setup, ["f2"])
        wait_for_console_pattern(self, "IA-64 EFI shell", vm=setup)
        setup.console_socket.sendall(b"edd30 off\r")
        wait_for_console_pattern(self, "EDD30 saved; reset to apply.", vm=setup)
        setup.shutdown()

        vm = self.get_vm(name="all-luns")
        vm.set_machine(f"hp-i2000,nvram={nvram}")
        vm.set_console()
        vm.add_args("-accel", "tcg", "-m", "2G", "-display", "none",
                    "-net", "none")
        for channel in range(2):
            for lun in range(32):
                for target in range(16):
                    if target == 7:
                        continue
                    name = f"disk-{channel}-{target}-{lun}"
                    vm.add_args("-drive", f"if=none,id={name},file={path},"
                                "format=raw,readonly=on",
                                "-device", f"scsi-hd,bus=isp12160-scsi.0,"
                                f"channel={channel},scsi-id={target},"
                                f"lun={lun},drive={name}")
        vm.launch()
        output = wait_for_console_pattern(
            self, "Press F2, F12, or Delete", vm=vm)
        self.assertNotIn(b"publication failed", output)
        self.assertIn(b"EDD drive numbers exhausted", output)
        self.send_keys(vm, ["f2"])
        wait_for_console_pattern(self, "IA-64 EFI shell", vm=vm)
        vm.console_socket.sendall(b"info\r")
        wait_for_console_pattern(self, "File systems:   960", vm=vm)
        paths = set()
        for index in (0, 95, 96, 479, 480, 959):
            vm.console_socket.sendall(f"boot fs{index}:\r".encode("ascii"))
            result = wait_for_suite(
                vm.console_socket, "smoke", SMOKE_CASES, 60.0,
                process_alive=vm.is_running)
            self.assertEqual(result.failed, 0, result.raw_console)
            nodes = self.device_path_nodes(result.raw_console)
            encoded = b"".join(nodes)
            self.assertNotIn(encoded, paths)
            paths.add(encoded)
            offset = 2
            if index >= 480:
                self.assertEqual(
                    nodes[offset], bytes.fromhex("0105080001000000"))
                offset += 1
            if index < 96:
                self.assertEqual(nodes[offset][0:2], b"\x01\x04")
                self.assertEqual(int.from_bytes(nodes[offset][20:24], "little"),
                                 0x80 + index)
            else:
                target = index % 15
                if target >= 7:
                    target += 1
                lun = (index % 480) // 15
                self.assertEqual(
                    nodes[offset], bytes([3, 2, 8, 0, target, 0, lun, 0]))
        vm.shutdown()

    def test_ide_device_positions(self):
        self.require_accelerator("tcg")
        for optical in (False, True):
            path = self.media_path(f"ide-positions-{optical}.img")
            if optical:
                make_el_torito_iso(path, app_path("smoke"))
            else:
                make_fat_disk(path, app_path("smoke"), layout="gpt")
            for channel in range(2):
                for unit in range(2):
                    vm = self.get_vm(name=f"ide-{optical}-{channel}-{unit}")
                    vm.set_machine("hp-i2000,nvram=none")
                    vm.set_console()
                    vm.add_args("-accel", "tcg", "-m", "2G", "-display", "none",
                                "-net", "none",
                                "-drive",
                                f"if=none,id=media,file={path},format=raw",
                                "-device", f"ide-{'cd' if optical else 'hd'},"
                                f"bus=ide.{channel},unit={unit},drive=media")
                    vm.launch()
                    result = wait_for_suite(
                        vm.console_socket, "smoke",
                        SMOKE_CASES | {"ide-dma-config"}, 60.0,
                        process_alive=vm.is_running)
                    self.assertEqual(result.failed, 0, result.raw_console)
                    nodes = self.device_path_nodes(result.raw_console)
                    self.assertEqual(
                        nodes[2], bytes([3, 1, 8, 0, channel, unit, 0, 0]))
                    vm.shutdown()

    def run_scsi_disk_boot(self, vga):
        self.require_accelerator("tcg")
        path = self.media_path(f"isp12160-{vga}.img")
        make_fat_disk(path, app_path("smoke"))

        vm = self.get_vm()
        vm.set_machine("hp-i2000,nvram=none")
        vm.set_console()
        vm.add_args(
            "-accel", "tcg",
            "-m", "2G",
            "-smp", "1",
            "-display", "none",
            "-net", "none",
            "-drive", f"file={path},format=raw",
            "-vga", vga,
        )
        vm.launch()

        result = wait_for_suite(
            vm.console_socket, "smoke", SMOKE_CASES, 60.0,
            process_alive=vm.is_running,
        )
        self.assertEqual(result.failed, 0)
        self.assertIn("SCSI controller:      ISP12160 polling",
                      result.raw_console)
        self.assertTrue(vm.is_running(), "QEMU exited after SCSI disk boot")

    def test_default_optical_boot(self):
        self.require_accelerator("tcg")
        trace_help = run([self.qemu_bin, "-d", "trace:help"],
                         capture_output=True, encoding="utf8")
        if (trace_help.returncode == 1 and
                trace_help.stdout.startswith("Log items (comma separated):")):
            self.skipTest("requires the log tracing backend")
        trace_help.check_returncode()
        if "ide_atapi_cmd_read" not in trace_help.stdout.splitlines():
            self.skipTest("requires the ide_atapi_cmd_read trace event")

        disk = self.media_path("blank-scsi.img")
        optical = self.media_path("optical.iso")
        trace = Path(self.scratch_file("ide.trace"))
        with disk.open("wb") as image:
            image.truncate(16 * 1024 * 1024)
        make_el_torito_iso(optical, app_path("smoke"), platform_id=0xEF)

        vm = self.get_vm()
        vm.set_machine("hp-i2000,nvram=none")
        vm.set_console()
        vm.add_args(
            "-accel", "tcg",
            "-m", "4G",
            "-smp", "1",
            "-display", "none",
            "-net", "none",
            "-drive", f"file={disk},format=raw",
            "-drive",
            f"file={optical},format=raw,media=cdrom,readonly=on",
            "-d", "trace:ide_atapi_cmd_read",
            "-D", str(trace),
        )
        vm.launch()

        result = wait_for_suite(
            vm.console_socket, "smoke", SMOKE_CASES, 60.0,
            process_alive=vm.is_running,
        )
        self.assertEqual(result.failed, 0)
        self.assertIn("read dma", trace.read_text())
        self.assertTrue(vm.is_running(), "QEMU exited after optical boot")

    def test_pci_root_protocols(self):
        self.require_accelerator("tcg")
        path = self.media_path("pci-root.img")
        make_fat_disk(path, app_path("i2000-pci-root"))

        vm = self.get_vm()
        vm.set_machine("hp-i2000,nvram=none")
        vm.set_console()
        vm.add_args(
            "-accel", "tcg",
            "-m", "2G",
            "-smp", "1",
            "-display", "none",
            "-vga", "quadro2",
            "-net", "none",
            "-drive", f"file={path},format=raw,if=scsi,index=0",
        )
        vm.launch()

        result = wait_for_suite(
            vm.console_socket, "i2000-pci-root", PCI_ROOT_CASES, 60.0,
            process_alive=vm.is_running,
        )
        self.assertEqual(result.failed, 0)
        self.assertTrue(vm.is_running(),
                        "QEMU exited after PCI root protocol test")

    def test_runtime_services(self):
        self.require_accelerator("tcg")
        path = self.media_path("runtime.img")
        make_fat_disk(path, app_path("i2000-runtime"))

        vm = self.get_vm()
        vm.set_machine("hp-i2000,nvram=none")
        vm.set_console()
        vm.add_args(
            "-accel", "tcg",
            "-m", "2G",
            "-smp", "1",
            "-display", "none",
            "-net", "none",
            "-drive", f"file={path},format=raw,if=scsi,index=0",
        )
        vm.launch()

        result = wait_for_suite(
            vm.console_socket, "i2000-runtime", RUNTIME_CASES, 60.0,
            process_alive=vm.is_running,
        )
        self.assertEqual(result.failed, 0)
        event = vm.event_wait("RESET", timeout=10.0)
        self.assertTrue(event["data"]["guest"])
        self.assertEqual(event["data"]["reason"], "guest-reset")
        self.assertTrue(vm.is_running(),
                        "QEMU exited after runtime service test")

    def test_graphics_protocols(self):
        self.require_accelerator("tcg")
        path = self.media_path("graphics.img")
        make_fat_disk(path, app_path("graphics"))

        vm = self.get_vm()
        vm.set_machine("hp-i2000,nvram=none")
        vm.set_console()
        vm.add_args(
            "-accel", "tcg",
            "-m", "2G",
            "-smp", "1",
            "-display", "none",
            "-vga", "quadro2",
            "-net", "none",
            "-drive", f"file={path},format=raw,if=scsi,index=0",
        )
        vm.launch()

        result = wait_for_suite(
            vm.console_socket, "graphics", GRAPHICS_CASES, 60.0,
            process_alive=vm.is_running,
        )
        self.assertEqual(result.failed, 0)
        self.assertTrue(vm.is_running(),
                        "QEMU exited after graphics protocol test")

    def test_keyboard_input(self):
        self.require_accelerator("tcg")
        path = self.media_path("input.img")
        make_fat_disk(path, app_path("input"))

        vm = self.get_vm()
        vm.set_machine("hp-i2000,nvram=none")
        vm.set_console()
        vm.add_args(
            "-accel", "tcg",
            "-m", "4G",
            "-smp", "1",
            "-display", "none",
            "-net", "none",
            "-drive", f"file={path},format=raw,if=scsi,index=0",
        )
        vm.launch()
        sent = set()

        def respond(case):
            if not case.passed or case.case_id in sent:
                return
            if case.case_id == "ready-basic":
                self.send_keys(vm, ("x",))
            elif case.case_id == "ready-modifier":
                self.send_keys(vm, ("shift", "a"))
            elif case.case_id == "ready-extended":
                self.send_keys(vm, ("up",))
            else:
                return
            sent.add(case.case_id)

        result = wait_for_suite(
            vm.console_socket, "input", INPUT_CASES, 45.0,
            on_case=respond, process_alive=vm.is_running,
        )
        self.assertEqual(result.failed, 0)
        self.assertEqual(
            sent, {"ready-basic", "ready-modifier", "ready-extended"})
        self.assertTrue(vm.is_running(),
                        "QEMU exited after keyboard input test")

    def test_loader_memory_layout(self):
        self.require_accelerator("tcg")
        path = self.media_path("loader.img")
        make_fat_disk(path, app_path("i2000-loader"))

        vm = self.get_vm()
        vm.set_machine("hp-i2000,nvram=none")
        vm.set_console()
        vm.add_args(
            "-accel", "tcg",
            "-m", "4G",
            "-smp", "2",
            "-display", "none",
            "-net", "none",
            "-drive", f"file={path},format=raw,if=scsi,index=0",
        )
        vm.launch()

        result = wait_for_suite(
            vm.console_socket, "i2000-loader", LOADER_CASES, 60.0,
            process_alive=vm.is_running,
        )
        self.assertEqual(result.failed, 0)
        self.assertTrue(vm.is_running(),
                        "QEMU exited after loader memory-layout test")

    def test_smp_rendezvous(self):
        self.require_accelerator("tcg")
        path = self.media_path("smp-merced.img")
        make_fat_disk(path, app_path("smp-merced"))

        vm = self.get_vm()
        vm.set_machine("hp-i2000,nvram=none")
        vm.set_console()
        vm.add_args(
            "-accel", "tcg,thread=multi",
            "-cpu", "merced",
            "-m", "4G",
            "-smp", "2",
            "-display", "none",
            "-net", "none",
            "-drive", f"file={path},format=raw,if=scsi,index=0",
        )
        vm.launch()

        result = wait_for_suite(
            vm.console_socket, "smp-merced", SMP_MERCED_CASES, 180.0,
            process_alive=vm.is_running,
        )
        self.assertEqual(result.failed, 0)
        self.assertSetEqual(set(result.cases), SMP_MERCED_CASES)
        self.assertTrue(vm.is_running(),
                        "QEMU exited after SMP rendezvous test")


if __name__ == "__main__":
    QemuSystemTest.main()
