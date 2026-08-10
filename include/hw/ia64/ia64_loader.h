/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Bounded IA-64 firmware image parsing helpers.
 */

#ifndef HW_IA64_LOADER_H
#define HW_IA64_LOADER_H

typedef struct IA64FirmwareEntrypoint {
    uint64_t entry;
    uint64_t global_pointer;
} IA64FirmwareEntrypoint;

bool ia64_loader_parse_pe_plabel(const void *image, size_t image_size,
                                 IA64FirmwareEntrypoint *result);

#endif /* HW_IA64_LOADER_H */
