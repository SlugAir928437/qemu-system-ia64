/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_IA64_PCI_H
#define HW_IA64_PCI_H

#include "hw/ia64/ia64_vpc_abi.h"

#define TYPE_IA64_PCI_HOST_BRIDGE "ia64-pcihost"

#define IA64_PCI_IO_BASE          IA64_LEGACY_IO_BASE
#define IA64_PCI_IO_SIZE          IA64_LEGACY_IO_PORTS_SIZE
#define IA64_PCI_IO_SPARSE_SIZE   IA64_LEGACY_IO_BLOCK_SIZE
#define IA64_PCI_CONFIG_BASE      0x0000007ff0000000ULL
#define IA64_PCI_CONFIG_SIZE      0x0000000010000000ULL

#if (IA64_PCI_IO_BASE & (IA64_PCI_IO_SPARSE_SIZE - 1)) != 0
#error "IA64_PCI_IO_BASE must be aligned for sparse I/O port addresses"
#endif

#define IA64_PCI_INTX_GSI_BASE 16
#define IA64_PCI_INTX_LINES    4

int ia64_pci_route_intx_gsi(uint8_t devfn, int irq_num);

#endif
