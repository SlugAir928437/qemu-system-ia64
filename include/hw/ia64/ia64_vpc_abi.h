/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 virtual PC ABI shared by QEMU and its freestanding firmware.
 *
 * This header must remain usable with -nostdinc.  Do not include QEMU or
 * hosted C library headers here; use compiler built-in types only.
 */

#ifndef HW_IA64_VPC_ABI_H
#define HW_IA64_VPC_ABI_H

#define IA64_FW_HANDOFF_ADDR          0x00000000000ff000ULL
#define IA64_FW_HANDOFF_MAGIC         0x4d41523436414951ULL /* "QIA64RAM" */
#define IA64_FW_HANDOFF_VERSION       10ULL

#define IA64_FW_COMPAT_HANDOFF_ADDR   (IA64_FW_HANDOFF_ADDR + 0x100ULL)
#define IA64_FW_COMPAT_HANDOFF_MAGIC  0x504d4f4334364951ULL /* "QI64COMP" */
#define IA64_FW_COMPAT_HANDOFF_VERSION 1ULL

#define IA64_FW_COMPAT_LOADER_DIRECT_ALIAS (1ULL << 0)
#define IA64_FW_COMPAT_EARLY_LOADER_MEMORY (1ULL << 1)
#define IA64_FW_COMPAT_SPARSE_SAL_MDT      (1ULL << 2)
#define IA64_FW_COMPAT_SAL_CODE_GP         (1ULL << 3)
#define IA64_FW_COMPAT_COMBINED_RUNTIME    (1ULL << 4)
#define IA64_FW_COMPAT_LEGACY_LOADER_MASK \
    (IA64_FW_COMPAT_LOADER_DIRECT_ALIAS | \
     IA64_FW_COMPAT_EARLY_LOADER_MEMORY | \
     IA64_FW_COMPAT_SPARSE_SAL_MDT | \
     IA64_FW_COMPAT_SAL_CODE_GP | \
     IA64_FW_COMPAT_COMBINED_RUNTIME)

#define IA64_FW_CONSOLE_SERIAL        0ULL
#define IA64_FW_CONSOLE_VGA           1ULL
#define IA64_FW_DEBUG_PORT_PRESENT    1ULL

#define IA64_VPC_MAX_CPUS             64U
#define IA64_FW_MIN_LOW_RAM_SIZE      0x0000000008000000ULL

/*
 * CPU-private physical memory used before and after ExitBootServices().
 *
 * Keep the assist area and the ordinary EFI stack pool in one reservation at
 * the end of installed low RAM.  All addresses below are offsets from the
 * reservation base so the low-memory allocation arena remains contiguous.
 */
#define IA64_FW_CPU_ASSIST_SIZE        0x0000000001000000ULL

#define IA64_FW_SAL_RUNTIME_OFFSET     0x0000000000000000ULL
#define IA64_FW_SAL_RUNTIME_SLOT_SIZE  0x0000000000008000ULL
#define IA64_FW_SAL_RUNTIME_END_OFFSET \
    (IA64_FW_SAL_RUNTIME_OFFSET + \
     IA64_VPC_MAX_CPUS * IA64_FW_SAL_RUNTIME_SLOT_SIZE)

#define IA64_FW_DEBUG_CONTEXT_OFFSET   0x0000000000200000ULL
#define IA64_FW_DEBUG_CONTEXT_STRIDE   0x0000000000000800ULL
#define IA64_FW_DEBUG_CONTEXT_SIZE     1192U
#define IA64_FW_DEBUG_CONTEXT_END_OFFSET \
    (IA64_FW_DEBUG_CONTEXT_OFFSET + \
     IA64_VPC_MAX_CPUS * IA64_FW_DEBUG_CONTEXT_STRIDE)

#define IA64_FW_DEBUG_STACK_OFFSET     0x0000000000300000ULL
#define IA64_FW_DEBUG_STACK_SIZE       0x0000000000008000ULL
#define IA64_FW_DEBUG_STACK_END_OFFSET \
    (IA64_FW_DEBUG_STACK_OFFSET + \
     IA64_VPC_MAX_CPUS * IA64_FW_DEBUG_STACK_SIZE)

#define IA64_FW_EARLY_RSE_OFFSET       0x0000000000600000ULL
#define IA64_FW_EARLY_RSE_SIZE         0x0000000000008000ULL
#define IA64_FW_EARLY_RSE_END_OFFSET \
    (IA64_FW_EARLY_RSE_OFFSET + \
     IA64_VPC_MAX_CPUS * IA64_FW_EARLY_RSE_SIZE)

#define IA64_FW_CPU_STACK_SIZE         0x0000000000020000ULL
#define IA64_FW_EFI_STACK_SIZE \
    (IA64_VPC_MAX_CPUS * IA64_FW_CPU_STACK_SIZE)
#define IA64_FW_BOOTSTRAP_STACK_TOP_OFFSET IA64_FW_CPU_ASSIST_SIZE
#define IA64_FW_FIXED_STACK_BASE_OFFSET \
    (IA64_FW_BOOTSTRAP_STACK_TOP_OFFSET - IA64_FW_EFI_STACK_SIZE)
#define IA64_FW_BOOT_STACK_SIZE \
    (IA64_FW_CPU_ASSIST_SIZE + IA64_FW_EFI_STACK_SIZE)

#define IA64_UART_BASE                0x00000047f0000000ULL
#define IA64_DEBUG_UART_BASE          0x00000047f0001000ULL
#define IA64_UART_MMIO_SIZE           0x0000000000002000ULL

#define IA64_PCI_MMIO_BASE            0x00000000c1000000ULL
#define IA64_PCI_MMIO_SIZE            0x0000000010000000ULL

#define IA64_LEGACY_IO_BASE           0x00000ffffc000000ULL
#define IA64_LEGACY_IO_PORTS_SIZE     0x0000000000010000ULL
#define IA64_LEGACY_IO_BLOCK_SIZE     0x0000000004000000ULL
#define IA64_LEGACY_IO_PORT_OFFSET(port) \
    ((((unsigned long long)(port) >> 2) << 12) | \
     ((unsigned long long)(port) & 0xfffULL))
#define IA64_LEGACY_IO_PORT_PA(port) \
    (IA64_LEGACY_IO_BASE + IA64_LEGACY_IO_PORT_OFFSET(port))

/*
 * The architected console resource is the conventional COM1 I/O range.
 * IA64_UART_BASE remains a private MMIO decode for firmware and compatibility
 * with older guests; ACPI and HCDP must advertise IA64_UART_IO_PORT instead.
 */
#define IA64_UART_IO_PORT             0x00000000000003f8ULL
#define IA64_UART_IO_SIZE             0x0000000000000008ULL

typedef struct __attribute__((packed)) IA64VpcHandoff {
    unsigned long long Magic;
    unsigned long long Version;
    unsigned long long RamSize;
    unsigned long long ConsolePolicy;
    unsigned long long IdeDmaEnabled;
    unsigned long long DebugPortFlags;
    unsigned long long DebugPortBase;
    unsigned long long I8042Enabled;
    unsigned long long ProcessorCount;
    unsigned long long NvramPersistent;
    unsigned long long SocketCount;
    unsigned long long CoresPerSocket;
    unsigned long long ThreadsPerCore;
} IA64VpcHandoff;

/*
 * Optional extension kept separate from IA64VpcHandoff so firmware that
 * understands only handoff version 10 continues to consume its base fields.
 * New firmware treats a missing extension as the historical compatibility
 * profile, preserving its behavior with older machine implementations.
 */
typedef struct __attribute__((packed)) IA64VpcCompatHandoff {
    unsigned long long Magic;
    unsigned long long Version;
    unsigned long long Size;
    unsigned long long Flags;
} IA64VpcCompatHandoff;

#endif /* HW_IA64_VPC_ABI_H */
