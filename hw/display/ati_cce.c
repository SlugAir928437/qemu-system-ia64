/*
 * ATI Rage128 Concurrent Command Engine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register and command interfaces follow the public Linux and X.Org drivers.
 * Technical references are listed in docs/devel/gpu-emulation-provenance.rst.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "ati_int.h"
#include "ati_regs.h"

#define CCE_GART_BASE (32 * MiB)
#define CCE_GART_SIZE (32 * MiB)
#define CCE_MAX_WORDS (1U << 20)
#define CCE_MAX_BLIT_WORK (2ULL * ATI_2D_MAX_PIXELS)
#define CCE_SLICE_DELAY_NS 1000
#define CCE_SCRATCH0 0x15e0

static uint32_t *cce_register(ATICCEState *c, hwaddr addr)
{
    switch (addr) {
    case PCI_GART_PAGE: return &c->pci_gart_page;
    case PM4_BUFFER_OFFSET: return &c->buffer_offset;
    case PM4_BUFFER_CNTL: return &c->buffer_cntl;
    case PM4_BUFFER_WM_CNTL: return &c->buffer_wm_cntl;
    case PM4_BUFFER_DL_RPTR_ADDR: return &c->rptr_addr;
    case PM4_BUFFER_DL_RPTR: return &c->rptr;
    case PM4_BUFFER_DL_WPTR: return &c->wptr;
    case PM4_IW_INDOFF: return &c->ind_offset;
    case PM4_IW_INDSIZE: return &c->ind_size;
    case PM4_MICRO_CNTL: return &c->micro_cntl;
    case GEN_RESET_CNTL: return &c->gen_reset_cntl;
    case CCE_SCRATCH0 ... CCE_SCRATCH0 + 5 * 4:
        return &c->scratch[(addr - CCE_SCRATCH0) / 4];
    default: return NULL;
    }
}

static uint32_t cce_status(const ATIVGAState *s)
{
    const ATICCEState *c = &s->cce;
    uint32_t status;

    switch (c->buffer_cntl >> 28) {
    case 1:
    case 2:
        status = 192;
        break;
    case 3:
    case 4:
        status = 128;
        break;
    case 5:
    case 6:
    case 7:
    case 8:
    case 15:
        status = 64;
        break;
    default:
        status = 0;
        break;
    }
    /* The end-of-stream flag is not part of the producer index. */
    if (c->processing || c->stream_count ||
        c->rptr != (c->wptr & ~PM4_BUFFER_DL_DONE)) {
        status |= PM4_STAT_BUSY;
    }
    if (s->host_data.active) {
        status |= BIT(31);
    }
    return status;
}

bool ati_cce_consume_work(ATIVGAState *s, uint64_t work, bool blit)
{
    ATICCEState *c = &s->cce;
    uint64_t *budget = blit ? &c->blit_budget : &c->command_budget;

    if (!c->processing) {
        return true;
    }
    if (work > *budget) {
        *budget = 0;
        c->budget_exhausted = true;
        return false;
    }
    *budget -= work;
    return true;
}

static bool cce_read_word(ATIVGAState *s, uint64_t addr, uint32_t *value)
{
    uint32_t entry, raw;
    uint64_t table;

    if ((addr & 3) || !ati_cce_consume_work(s, 1, false)) {
        return false;
    }
    if (addr < CCE_GART_BASE) {
        if (addr + 4 > s->vga.vram_size) {
            return false;
        }
        *value = ldl_le_p(s->vga.vram_ptr + addr);
        return true;
    }
    if (addr >= CCE_GART_BASE + CCE_GART_SIZE ||
        (s->cce.pci_gart_page & 1)) {
        return false;
    }
    addr -= CCE_GART_BASE;
    table = s->cce.pci_gart_page & ~0xfffU;
    if (pci_dma_read(&s->dev, table + (addr >> 12) * 4,
                     &entry, sizeof(entry)) != MEMTX_OK) {
        return false;
    }
    addr = (le32_to_cpu(entry) & ~0xfffU) | (addr & 0xfff);
    if (pci_dma_read(&s->dev, addr, &raw, sizeof(raw)) != MEMTX_OK) {
        return false;
    }
    *value = le32_to_cpu(raw);
    return true;
}

static bool cce_stream_read(ATIVGAState *s, ATICCEStream *stream,
                            uint32_t *value)
{
    if (!stream->remaining ||
        !cce_read_word(s, stream->base + (uint64_t)stream->pos * 4, value)) {
        return false;
    }
    stream->pos = (stream->pos + 1) & stream->mask;
    stream->remaining--;
    return true;
}

static unsigned int cce_packet_count(uint32_t header)
{
    unsigned int type = header >> 30;

    return type == 2 ? 0 : type == 1 ? 2 : extract32(header, 16, 14) + 1;
}

static bool cce_process_word(ATIVGAState *s, ATICCEStream *stream)
{
    ATICCEState *c = &s->cce;
    ATICCEStream next = *stream;
    uint64_t generation = s->cce_generation;
    uint32_t value;
    unsigned int type;
    hwaddr reg;

    if (!cce_stream_read(s, &next, &value) ||
        generation != s->cce_generation) {
        return false;
    }
    if (!stream->packet_count) {
        type = value >> 30;
        next.header = value;
        next.packet_count = cce_packet_count(value);
        next.packet_index = 0;
        if (next.packet_count > next.remaining) {
            return false;
        }
        if (type == 3 && extract32(value, 8, 8) != 0x10) {
            qemu_log_mask(LOG_UNIMP, "ati: unsupported CCE packet 0x%08x\n",
                          value);
            return false;
        }
        *stream = next;
        if (!next.packet_count && c->stream_count == 1) {
            c->rptr = next.pos;
        }
        return true;
    }

    next.packet_index++;
    type = stream->header >> 30;
    if (type == 3) {
        *stream = next;
        return true;
    }
    if (type == 1) {
        reg = extract32(stream->header, stream->packet_index * 11, 11) * 4;
    } else {
        reg = (stream->header & 0x7ffU) * 4;
        if (!(stream->header & BIT(15))) {
            reg += stream->packet_index * 4;
        }
    }
    if (reg >= ATI_RAGE128_MMIO_SIZE ||
        !ati_cce_consume_work(s, 1, false)) {
        return false;
    }
    if (reg == PM4_IW_INDSIZE) {
        unsigned int mode = c->buffer_cntl >> 28;

        if (mode < 3 || mode > 8 || value > CCE_MAX_WORDS ||
            c->stream_count == ARRAY_SIZE(c->streams)) {
            return false;
        }
        c->ind_size = value;
        *stream = next;
        c->streams[c->stream_count++] = (ATICCEStream) {
            .base = CCE_GART_BASE + (uint64_t)c->ind_offset,
            .remaining = value,
            .mask = UINT32_MAX,
        };
        return true;
    }

    ati_mmio_write(s, reg, value, 4);
    if (generation != s->cce_generation) {
        return false;
    }
    *stream = next;
    /* An operation that exceeded its own limit must not be replayed. */
    c->halted = c->budget_exhausted;
    return !c->halted;
}

static bool cce_enabled(const ATICCEState *c)
{
    unsigned int mode = c->buffer_cntl >> 28;

    return !c->halted && (c->micro_cntl & PM4_MICRO_FREERUN) &&
           mode >= 2 && mode <= 8 && !(mode & 1) &&
           (c->buffer_cntl & 0x07ffffff) < 20;
}

static void cce_schedule(ATIVGAState *s)
{
    if (cce_enabled(&s->cce)) {
        timer_mod(&s->cce_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + CCE_SLICE_DELAY_NS);
    }
}

static void cce_process_ring(ATIVGAState *s)
{
    ATICCEState *c = &s->cce;
    unsigned int order = c->buffer_cntl & 0x07ffffff;
    uint32_t wptr = c->wptr & ~PM4_BUFFER_DL_DONE;
    uint64_t generation = s->cce_generation;
    uint64_t command_work = CCE_MAX_WORDS;
    uint64_t blit_work = ATI_2D_MAX_PIXELS;
    uint32_t mask;

    if (c->processing) {
        return;
    }
    timer_del(&s->cce_timer);
    if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF || !cce_enabled(c)) {
        return;
    }
    mask = (1U << (order + 1)) - 1;
    if (c->rptr > mask || wptr > mask) {
        return;
    }
    if (!c->stream_count) {
        c->streams[0] = (ATICCEStream) {
            .base = c->buffer_offset,
            .pos = c->rptr,
            .mask = mask,
        };
        c->stream_count = 1;
    }
    c->processing = true;
    while (c->stream_count && generation == s->cce_generation) {
        ATICCEStream *stream = &c->streams[c->stream_count - 1];

        if (stream->packet_count &&
            stream->packet_index == stream->packet_count) {
            stream->packet_count = 0;
            stream->packet_index = 0;
            if (c->stream_count == 1) {
                c->rptr = stream->pos;
            }
        }
        if (c->stream_count == 1) {
            wptr = c->wptr & ~PM4_BUFFER_DL_DONE;
            if (wptr > mask) {
                break;
            }
            stream->remaining = (wptr - stream->pos) & mask;
        }
        if (!stream->remaining) {
            if (stream->packet_count) {
                break;
            }
            c->stream_count--;
            continue;
        }
        if (!cce_enabled(c)) {
            break;
        }
        if (!command_work || !blit_work) {
            cce_schedule(s);
            break;
        }
        /* Allow a pending HOST_DATA flush and a new draw in one write. */
        c->command_budget = CCE_MAX_WORDS;
        c->blit_budget = CCE_MAX_BLIT_WORK;
        c->budget_exhausted = false;
        if (!cce_process_word(s, stream)) {
            break;
        }
        command_work -= MIN(command_work, CCE_MAX_WORDS - c->command_budget);
        blit_work -= MIN(blit_work, CCE_MAX_BLIT_WORK - c->blit_budget);
    }
    c->processing = false;
    c->command_budget = 0;
    c->blit_budget = 0;
}

static void cce_resume(void *opaque)
{
    ATIVGAState *s = opaque;
    MemReentrancyGuard *guard = &DEVICE(s)->mem_reentrancy_guard;
    bool engaged = guard->engaged_in_io;

    guard->engaged_in_io = true;
    cce_process_ring(s);
    guard->engaged_in_io = engaged;
}

static void cce_discard_streams(ATIVGAState *s)
{
    timer_del(&s->cce_timer);
    memset(s->cce.streams, 0, sizeof(s->cce.streams));
    s->cce.stream_count = 0;
    s->cce.halted = false;
    s->cce_generation++;
}

bool ati_cce_read(ATIVGAState *s, hwaddr addr, uint64_t *data,
                  unsigned int size)
{
    hwaddr base = addr & ~3ULL;
    uint32_t *reg, value;

    if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF || (addr & 3) + size > 4) {
        return false;
    }
    reg = cce_register(&s->cce, base);
    if (base == PM4_STAT) {
        value = cce_status(s);
    } else if (reg) {
        value = *reg;
    } else {
        return false;
    }
    *data = extract32(value, (addr & 3) * 8, size * 8);
    return true;
}

bool ati_cce_write(ATIVGAState *s, hwaddr addr, uint64_t data,
                   unsigned int size)
{
    hwaddr base = addr & ~3ULL;
    ATICCEState *c = &s->cce;
    uint32_t *reg;
    uint32_t old;

    if (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF || (addr & 3) + size > 4) {
        return false;
    }
    reg = cce_register(c, base);
    if (!reg) {
        return base == PM4_STAT;
    }
    old = *reg;
    *reg = deposit32(old, (addr & 3) * 8, size * 8, data);
    if (base == GEN_RESET_CNTL && (c->gen_reset_cntl & SOFT_RESET_GUI)) {
        c->micro_cntl = 0;
        cce_discard_streams(s);
        memset(&s->host_data, 0, sizeof(s->host_data));
    } else {
        if (*reg != old && (base == PM4_BUFFER_OFFSET ||
                            base == PM4_BUFFER_CNTL ||
                            base == PM4_BUFFER_DL_RPTR)) {
            cce_discard_streams(s);
        }
        if (base == PM4_BUFFER_DL_WPTR || base == PM4_MICRO_CNTL ||
            base == PM4_BUFFER_CNTL || base == PM4_BUFFER_OFFSET ||
            base == PM4_BUFFER_DL_RPTR) {
            if (c->processing) {
                cce_schedule(s);
            } else {
                cce_process_ring(s);
            }
        }
    }
    return true;
}

void ati_cce_reset(ATIVGAState *s)
{
    timer_del(&s->cce_timer);
    memset(&s->cce, 0, sizeof(s->cce));
    s->cce_generation++;
}

void ati_cce_init(ATIVGAState *s)
{
    timer_init_ns(&s->cce_timer, QEMU_CLOCK_VIRTUAL, cce_resume, s);
}

int ati_cce_post_load(ATIVGAState *s)
{
    ATICCEState *c = &s->cce;
    unsigned int order = c->buffer_cntl & 0x07ffffff;

    s->cce_generation++;
    c->processing = false;
    c->command_budget = c->blit_budget = 0;
    c->budget_exhausted = false;
    if (c->stream_count > ARRAY_SIZE(c->streams) ||
        (c->stream_count &&
         (s->dev_id != PCI_DEVICE_ID_ATI_RAGE128_PF || order >= 20))) {
        return -EINVAL;
    }
    for (unsigned int i = 0; i < c->stream_count; i++) {
        const ATICCEStream *stream = &c->streams[i];
        uint32_t mask = i ? UINT32_MAX : (1U << (order + 1)) - 1;

        if (stream->mask != mask ||
            stream->pos > mask || stream->remaining > mask ||
            stream->packet_index > stream->packet_count ||
            (stream->packet_count &&
             (stream->packet_count != cce_packet_count(stream->header) ||
              ((stream->header >> 30) == 3 &&
               extract32(stream->header, 8, 8) != 0x10))) ||
            (i && (stream->pos > CCE_MAX_WORDS ||
                   stream->remaining > CCE_MAX_WORDS - stream->pos)) ||
            (!i && (stream->base != c->buffer_offset || c->rptr > mask))) {
            return -EINVAL;
        }
    }
    return 0;
}

static const VMStateDescription vmstate_ati_cce_stream = {
    .name = "ati-vga/cce/stream",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(base, ATICCEStream),
        VMSTATE_UINT32(pos, ATICCEStream),
        VMSTATE_UINT32(remaining, ATICCEStream),
        VMSTATE_UINT32(mask, ATICCEStream),
        VMSTATE_UINT32(header, ATICCEStream),
        VMSTATE_UINT32(packet_count, ATICCEStream),
        VMSTATE_UINT32(packet_index, ATICCEStream),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_ati_cce = {
    .name = "ati-vga/cce",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pci_gart_page, ATICCEState),
        VMSTATE_UINT32(buffer_offset, ATICCEState),
        VMSTATE_UINT32(buffer_cntl, ATICCEState),
        VMSTATE_UINT32(buffer_wm_cntl, ATICCEState),
        VMSTATE_UINT32(rptr_addr, ATICCEState),
        VMSTATE_UINT32(rptr, ATICCEState),
        VMSTATE_UINT32(wptr, ATICCEState),
        VMSTATE_UINT32(ind_offset, ATICCEState),
        VMSTATE_UINT32(ind_size, ATICCEState),
        VMSTATE_UINT32(micro_cntl, ATICCEState),
        VMSTATE_UINT32_ARRAY(scratch, ATICCEState, 6),
        VMSTATE_UINT32(gen_reset_cntl, ATICCEState),
        VMSTATE_STRUCT_ARRAY(streams, ATICCEState, ATI_CCE_MAX_DEPTH + 1, 0,
                             vmstate_ati_cce_stream, ATICCEStream),
        VMSTATE_UINT32(stream_count, ATICCEState),
        VMSTATE_BOOL(halted, ATICCEState),
        VMSTATE_END_OF_LIST()
    },
};
