/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * IA-64 CPU QOM and execution-engine glue.
 *
 * Instruction decoding, family generators, and architectural helper logic
 * live in decode/, translate/, and arch/ respectively.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "arch/arch.h"
#include "ia32/ia32.h"
#include "debug.h"
#include "translate/translate.h"
#include "exec/cputlb.h"
#include "exec/cpu-common.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "hw/core/sysemu-cpu-ops.h"
#include "accel/tcg/cpu-ops.h"
#include "tcg/debug-assert.h"
#include "exec/translator.h"
#include "exec/helper-proto.h"
#include "system/memory.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

static void ia64_cpu_set_pc(CPUState *cs, vaddr value)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    if (cpu->env.psr & IA64_PSR_IS) {
        cpu->env.ia32.eip =
            (uint32_t)(value - cpu->env.ia32.segs[R_CS].base);
    }
    cpu->env.ip = value;
}

static vaddr ia64_cpu_get_pc(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    return cpu->env.psr & IA64_PSR_IS ?
           ia64_ia32_virtual_ip(&cpu->env) : cpu->env.ip;
}


static TCGTBCPUState ia64_get_tb_cpu_state(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint64_t psr = cpu->env.psr;
    CPUX86State *xenv = &cpu->env.ia32;

    if (psr & IA64_PSR_IS) {
        uint32_t cs_base = xenv->segs[R_CS].base;
        uint32_t flags = xenv->hflags |
            (xenv->eflags &
             (IOPL_MASK | TF_MASK | RF_MASK | VM_MASK | AC_MASK)) |
            ((psr & IA64_PSR_DB) ? IA64_TB_FLAG_IA32_PSR_DB : 0) |
            ((psr & IA64_PSR_AC) ? IA64_TB_FLAG_IA32_PSR_AC : 0) |
            ((psr & IA64_PSR_SS) ? TF_MASK : 0);

        return (TCGTBCPUState) {
            .pc = (uint32_t)(cs_base + xenv->eip),
            .cs_base = cs_base,
            .flags = flags | IA64_TB_FLAG_PSR_IS,
        };
    }

    uint32_t flags =
        ((psr >> 17) & IA64_TB_FLAG_DT) |
        ((psr >> 35) & IA64_TB_FLAG_IT) |
        ((psr >> (IA64_PSR_RI_SHIFT - IA64_TB_FLAG_RI_SHIFT)) &
         IA64_TB_FLAG_RI_MASK) |
        ((psr >> 8) & IA64_TB_FLAG_PSR_IC) |
        ((psr << 5) & IA64_TB_FLAG_BE) |
        ((uint32_t)cpu->env.instruction_group_start << 7) |
        ((psr >> (IA64_PSR_CPL_SHIFT - IA64_TB_FLAG_CPL_SHIFT)) &
         IA64_TB_FLAG_CPL_MASK);

    flags |= (psr & IA64_PSR_FAULT_SUPPRESS_MASK) != 0 ?
             IA64_TB_FLAG_PSR_SUPPRESS : 0;

    return (TCGTBCPUState) {
        .pc = cpu->env.ip,
        .flags = flags,
    };
}

void ia64_tlb_bump_generation(CPUIA64State *env, bool is_ifetch)
{
    IA64MicroTlbEntry *micro = is_ifetch ? env->mmu.tlb_inst_micro :
                                           env->mmu.tlb_data_micro;
    uint32_t *generation = is_ifetch ? &env->mmu.tlb_inst_generation :
                                       &env->mmu.tlb_data_generation;

    (*generation)++;
    if (*generation == 0) {
        *generation = 1;
        memset(micro, 0, sizeof(*micro) * IA64_MICRO_TLB_SIZE);
    }
}

void ia64_tlb_bump_slot_generation(CPUIA64State *env, bool is_ifetch,
                                   uint16_t slot)
{
    IA64TlbEntry *tlb = is_ifetch ? env->mmu.tlb_inst :
                                    env->mmu.tlb_data;

    g_assert(slot < IA64_TLB_MAX);
    if (++tlb[slot].micro_generation == 0) {
        /*
         * A wrapped slot version could validate a very old hint.  Make the
         * wrap unambiguous by invalidating all hints before reusing version
         * one.  This path requires over four billion changes to one slot.
         */
        tlb[slot].micro_generation = 1;
        ia64_tlb_bump_generation(env, is_ifetch);
    }
}

const IA64TlbEntry *ia64_tlb_find_slow(CPUIA64State *env, uint64_t va,
                                       uint32_t rid, bool is_ifetch)
{
    IA64TlbEntry *tlb = is_ifetch ? env->mmu.tlb_inst : env->mmu.tlb_data;
    IA64MicroTlbEntry *micro = is_ifetch ? env->mmu.tlb_inst_micro :
                                           env->mmu.tlb_data_micro;
    uint16_t tlb_count = is_ifetch ? env->mmu.tlb_inst_count :
                                     env->mmu.tlb_data_count;
    uint32_t generation = is_ifetch ? env->mmu.tlb_inst_generation :
                                      env->mmu.tlb_data_generation;
    uint16_t i;

    /*
     * Merced's DTLB1 and DTLB2 are non-inclusive.  Although DTLB1 is not
     * architecturally enumerated as additional TR/TC storage, an entry
     * cached there continues to translate accesses after its DTLB2 source
     * has been replaced.  The host softmmu TLB is only an optimization and
     * may be flushed independently, so it cannot stand in for this lookup.
     */
    if (!is_ifetch &&
        ia64_env_cpu_class(env)->model == IA64_CPU_MODEL_MERCED) {
        int cached = ia64_merced_dtlb1_lookup(env, va, rid);

        if (cached >= 0) {
            return &env->mmu.tlb_data_l1[cached];
        }
        for (i = 0; i < IA64_DTLB1_MAX; i++) {
            IA64TlbEntry *entry = &env->mmu.tlb_data_l1[i];

            if (ia64_tlb_match(entry, va, rid)) {
                return entry;
            }
        }
    }

    for (i = 0; i < tlb_count; i++) {
        IA64TlbEntry *entry = &tlb[i];

        if (ia64_tlb_match(entry, va, rid)) {
            micro[ia64_micro_tlb_index(va, rid)] = (IA64MicroTlbEntry) {
                .va = entry->va,
                .page_mask = entry->page_mask,
                .pte = entry->pte,
                .rid = entry->rid,
                .generation = generation,
                .slot_generation = entry->micro_generation,
                .slot = i,
                .valid = true,
            };
            return entry;
        }
    }
    return NULL;
}

static void ia64_cpu_synchronize_from_tb(CPUState *cs,
                                         const TranslationBlock *tb)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    if (tb->flags & IA64_TB_FLAG_PSR_IS) {
        tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
        cpu->env.ia32.eip = (uint32_t)(tb->pc - tb->cs_base);
        cpu->env.ip = (uint32_t)tb->pc;
        return;
    }

    uint64_t ri =
        (tb->flags & IA64_TB_FLAG_RI_MASK) >> IA64_TB_FLAG_RI_SHIFT;

    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu->env.ip = tb->pc;
    /*
     * Translation-time instruction fetch faults occur before generated TCG
     * can update PSR.ri.  Restore the slot encoded in the TB key along with
     * its bundle address; otherwise a stale slot from the preceding TB is
     * saved in IPSR and rfi can skip the faulting bundle's prologue.
     */
    cpu->env.psr = (cpu->env.psr & ~IA64_PSR_RI_MASK) |
                   (ri << IA64_PSR_RI_SHIFT);
}

static void ia64_restore_state_to_opc(CPUState *cs,
                                       const TranslationBlock *tb,
                                       const uint64_t *data)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    if (tb->flags & IA64_TB_FLAG_PSR_IS) {
        CPUX86State *xenv = &cpu->env.ia32;
        uint64_t new_pc;

        if (tb_cflags(tb) & CF_PCREL) {
            uint64_t pc = xenv->eip + tb->cs_base;

            new_pc = (pc & TARGET_PAGE_MASK) | data[0];
        } else {
            new_pc = data[0];
        }
        xenv->eip = (uint32_t)(new_pc - tb->cs_base);
        cpu->env.ip = (uint32_t)new_pc;
        if (data[1] != CC_OP_DYNAMIC) {
            xenv->cc_op = data[1];
        }
        return;
    }

    cpu->env.ip = data[0];
}

static int ia64_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    if (cpu->env.psr & (ifetch ? IA64_PSR_IT : IA64_PSR_DT)) {
        return MMU_IDX_VIRT_CPL(ia64_psr_cpl(cpu->env.psr));
    }
    return MMU_PHYS_IDX;
}

static vaddr ia64_pointer_wrap(CPUState *cs, int mmu_idx,
                               vaddr result, vaddr base)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    return cpu->env.psr & IA64_PSR_IS ? (uint32_t)result : result;
}


static int ia64_tlb_perm_to_prot(uint8_t perm)
{
    int prot = 0;

    if (perm & IA64_TLB_R) {
        prot |= PAGE_READ;
    }
    if (perm & IA64_TLB_W) {
        prot |= PAGE_WRITE;
    }
    if (perm & IA64_TLB_X) {
        prot |= PAGE_EXEC;
    }
    return prot;
}

static int ia64_tlb_prot_for_pte_psr(uint64_t pte, uint8_t perm,
                                     bool is_ifetch, uint64_t psr)
{
    int prot = ia64_tlb_perm_to_prot(perm);

    /* IA-64 has independent instruction and data translation caches. */
    prot &= is_ifetch ? PAGE_EXEC : (PAGE_READ | PAGE_WRITE);

    /*
     * QEMU's software TLB may satisfy later accesses without re-entering
     * tlb_fill.  Do not cache write permission for a clean IA-64 PTE: a
     * later store must take Data Dirty so the OS can update the PTE or break
     * copy-on-write sharing.
     */
    if (!is_ifetch && !(psr & IA64_PSR_DA)) {
        if (!(pte & IA64_PTE_ACCESSED)) {
            prot &= ~(PAGE_READ | PAGE_WRITE);
        }
        if (!(pte & IA64_PTE_DIRTY)) {
            prot &= ~PAGE_WRITE;
        }
    } else if (is_ifetch && !(psr & IA64_PSR_IA) &&
               !(pte & IA64_PTE_ACCESSED)) {
        prot &= ~PAGE_EXEC;
    }

    return prot;
}

static int ia64_tlb_prot_for_pte(CPUIA64State *env, uint64_t pte,
                                 uint8_t perm, bool is_ifetch)
{
    return ia64_tlb_prot_for_pte_psr(pte, perm, is_ifetch, env->psr);
}

static void ia64_record_suppressed_tlb_fill(CPUIA64State *env, vaddr addr,
                                             int mmu_idx)
{
    uint64_t page = addr & TARGET_PAGE_MASK;
    uint16_t idxmap = 1u << mmu_idx;
    uint8_t i;

    for (i = 0; i < env->exception_state.suppressed_tlb_count; i++) {
        if (env->exception_state.suppressed_tlb_pages[i] == page) {
            env->exception_state.suppressed_tlb_idxmaps[i] |= idxmap;
            return;
        }
    }

    if (env->exception_state.suppressed_tlb_count == IA64_SUPPRESSED_TLB_MAX) {
        env->exception_state.suppressed_tlb_overflow = true;
        return;
    }

    i = env->exception_state.suppressed_tlb_count++;
    env->exception_state.suppressed_tlb_pages[i] = page;
    env->exception_state.suppressed_tlb_idxmaps[i] = idxmap;
}

static void ia64_record_suppressed_tlb_fill_if_needed(
    CPUIA64State *env, vaddr addr, int mmu_idx, uint64_t pte, uint8_t perm,
    bool is_ifetch, int prot)
{
    uint64_t unsuppressed_psr = env->psr & ~(IA64_PSR_DA | IA64_PSR_IA);
    int unsuppressed_prot;

    if (!(env->psr & (IA64_PSR_DA | IA64_PSR_IA))) {
        return;
    }

    unsuppressed_prot = ia64_tlb_prot_for_pte_psr(
        pte, perm, is_ifetch, unsuppressed_psr);
    if (prot != unsuppressed_prot) {
        ia64_record_suppressed_tlb_fill(env, addr, mmu_idx);
    }
}

void ia64_flush_suppressed_tlb(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);
    uint8_t i;

    if (env->exception_state.suppressed_tlb_overflow) {
        tlb_flush(cs);
    } else {
        for (i = 0; i < env->exception_state.suppressed_tlb_count; i++) {
            tlb_flush_page_by_mmuidx(
                cs, env->exception_state.suppressed_tlb_pages[i],
                env->exception_state.suppressed_tlb_idxmaps[i]);
        }
    }

    env->exception_state.suppressed_tlb_count = 0;
    env->exception_state.suppressed_tlb_overflow = false;
}

static void ia64_tlb_set_entry_page(CPUState *cs, vaddr addr, hwaddr pa,
                                    uint64_t page_size, int prot, int mmu_idx,
                                    IA64MemorySpeculation speculation,
                                    uint8_t memory_attribute)
{
    CPUTLBEntryFull full = {
        .phys_addr = pa & TARGET_PAGE_MASK,
        .attrs = MEMTXATTRS_UNSPECIFIED,
        .prot = prot,
        .lg_page_size = TARGET_PAGE_BITS,
    };

    (void)page_size;
    full.extra.ia64.speculation = speculation;
    full.extra.ia64.memory_attribute = memory_attribute;
    tlb_set_page_full(cs, mmu_idx, addr & TARGET_PAGE_MASK, &full);
}

static hwaddr ia64_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint64_t pa;

    if (!ia64_mmu_translate_debug(&cpu->env, addr, &pa)) {
        return -1;
    }
    return pa & TARGET_PAGE_MASK;
}

static bool ia64_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                              MMUAccessType access_type, int mmu_idx,
                              bool probe, uintptr_t retaddr)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    bool is_ifetch = (access_type == MMU_INST_FETCH);
    uint8_t needed = is_ifetch ? IA64_TLB_X :
                     (access_type == MMU_DATA_STORE ? IA64_TLB_W :
                      IA64_TLB_R);
    uint64_t pa;
    uint8_t perm;
    uint32_t rid;
    IA64Exception excp;
    bool is_rse = !is_ifetch && mmu_idx == MMU_IDX_RSE;
    uint8_t access_level;
    bool virt_translation_enabled;

    if (!probe && is_ifetch && (cpu->env.psr & IA64_PSR_IS) &&
        (uint32_t)addr == ia64_ia32_virtual_ip(&cpu->env)) {
        /*
         * The first executable-page lookup happens before x86 decoding.
         * Preserve the architectural ordering of IA-32 instruction
         * breakpoint and code-fetch faults ahead of instruction TLB faults.
         */
        ia64_ia32_check_fetch_fault_priority(&cpu->env, addr, 0);
    }

    rid = ia64_region_rid(&cpu->env, addr);
    if (mmu_idx == MMU_PHYS_IDX) {
        if (!ia64_pa_is_implemented(&cpu->env, addr)) {
            if (probe) {
                return false;
            }
            excp = is_ifetch ? IA64_EXCP_UNIMPL_INST_ADDR :
                   IA64_EXCP_UNIMPL_DATA_ADDR;
            if (is_ifetch) {
                cpu->env.ip = ia64_pa_canonicalize(&cpu->env, addr);
            }
            goto raise_exception;
        }
        pa = ia64_physical_address(addr);
        ia64_tlb_set_entry_page(
            cs, addr, pa, TARGET_PAGE_SIZE,
            PAGE_READ | PAGE_WRITE | PAGE_EXEC, mmu_idx,
            (addr & IA64_PHYS_UC_BIT) ? IA64_MEM_NON_SPECULATIVE :
                                       IA64_MEM_LIMITED_SPECULATION,
            (addr & IA64_PHYS_UC_BIT) ? 4 : 0);
        return true;
    }

    if (is_rse) {
        access_level = ia64_rsc_pl(cpu->env.ar_rsc);
    } else {
        g_assert(mmu_idx >= MMU_IDX_VIRT_CPL0 &&
                 mmu_idx <= MMU_IDX_VIRT_CPL3);
        access_level = mmu_idx - MMU_IDX_VIRT_CPL0;
    }

    /* A translated MMU index is itself the serialized translation state. */
    virt_translation_enabled = true;
    if (virt_translation_enabled &&
        !ia64_va_is_implemented(&cpu->env, addr)) {
        if (probe) {
            return false;
        }
        excp = is_ifetch ? IA64_EXCP_UNIMPL_INST_ADDR :
               IA64_EXCP_UNIMPL_DATA_ADDR;
        if (is_ifetch) {
            cpu->env.ip = ia64_va_canonicalize(&cpu->env, addr);
        }
        goto raise_exception;
    }

    if (ia64_firmware_identity_pa(cpu->env.cr_iva,
                                  is_ifetch ? addr : cpu->env.ip,
                                  cpu->env.psr, addr, &pa)) {
        int prot = is_ifetch ? PAGE_EXEC : (PAGE_READ | PAGE_WRITE);

        ia64_tlb_set_entry_page(cs, addr, pa, TARGET_PAGE_SIZE, prot,
                                mmu_idx, IA64_MEM_SPECULATIVE, 0);
        return true;
    }

    {
        const IA64TlbEntry *entry = ia64_tlb_find_cached(
            &cpu->env, addr, rid, is_ifetch);

        if (entry) {
            int prot;
            IA64Exception pte_excp;

            ia64_tlb_entry_translate(entry, addr, access_level, &pa, &perm);
            pte_excp = ia64_tlb_exception_for_access(
                &cpu->env, entry, perm, needed, is_ifetch,
                access_type == MMU_DATA_STORE, is_rse);
            if (pte_excp != IA64_EXCP_NONE) {
                if (probe) {
                    return false;
                }
                excp = pte_excp;
                goto raise_exception;
            }
            prot = ia64_tlb_prot_for_pte(&cpu->env, entry->pte, perm,
                                         is_ifetch);
            ia64_record_suppressed_tlb_fill_if_needed(
                &cpu->env, addr, mmu_idx, entry->pte, perm, is_ifetch, prot);
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 tlb hit %c va=0x%016" PRIx64
                          " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                          " perm=0x%x\n",
                          is_ifetch ? 'i' : 'd', (uint64_t)addr, rid, pa,
                          perm);
            /*
             * An L1 replacement can flush a derived softmmu entry.  Do it
             * before installing this access's host entry so the fill caller
             * never resumes through an entry invalidated during the fill.
             */
            if (!is_ifetch && !probe) {
                ia64_mmu_data_access(&cpu->env, addr, size, true);
            }
            ia64_tlb_set_entry_page(
                cs, addr, pa, entry->ps, prot, mmu_idx,
                ia64_pte_memory_speculation(entry->pte),
                (entry->pte >> 2) & 7);
            return true;
        }
    }

    if (!is_ifetch) {
        const IA64TlbEntry *new_entry;
        uint64_t pte = 0;
        uint32_t key = 0;

        if (ia64_vhpt_walk_full(&cpu->env, addr, rid, false, is_rse,
                                access_level, &pa, &perm, &pte, &key,
                                &new_entry)) {
            int prot;
            IA64Exception pte_excp;
            uint64_t page_size = new_entry ? new_entry->ps : TARGET_PAGE_SIZE;

            pte_excp = new_entry ?
                ia64_tlb_exception_for_access(
                    &cpu->env, new_entry, perm, needed, false,
                    access_type == MMU_DATA_STORE, is_rse) :
                ia64_translation_exception_for_access(
                    &cpu->env, pte, key, perm, needed, false,
                    access_type == MMU_DATA_STORE, is_rse);
            if (pte_excp != IA64_EXCP_NONE) {
                if (probe) {
                    return false;
                }
                excp = pte_excp;
                goto raise_exception;
            }
            prot = ia64_tlb_prot_for_pte(&cpu->env,
                                         new_entry ? new_entry->pte : pte,
                                         perm, false);
            ia64_record_suppressed_tlb_fill_if_needed(
                &cpu->env, addr, mmu_idx,
                new_entry ? new_entry->pte : pte, perm, false, prot);
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt hit d va=0x%016" PRIx64
                          " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                          " perm=0x%x iha=0x%016" PRIx64 "\n",
                          (uint64_t)addr, rid, pa, perm,
                          ia64_vhpt_hash_address(&cpu->env, addr));
            if (!probe) {
                ia64_mmu_data_access(&cpu->env, addr, size, true);
            }
            ia64_tlb_set_entry_page(
                cs, addr, pa, page_size, prot, mmu_idx,
                ia64_pte_memory_speculation(new_entry ? new_entry->pte :
                                                        pte),
                ((new_entry ? new_entry->pte : pte) >> 2) & 7);
            return true;
        }
    }

    if (is_ifetch) {
        const IA64TlbEntry *new_entry;
        uint64_t pte = 0;
        uint32_t key = 0;

        if (ia64_vhpt_walk_full(&cpu->env, addr, rid, true, false,
                                access_level, &pa, &perm, &pte, &key,
                                &new_entry)) {
            int prot;
            IA64Exception pte_excp;
            uint64_t page_size = new_entry ? new_entry->ps : TARGET_PAGE_SIZE;

            pte_excp = new_entry ?
                ia64_tlb_exception_for_access(
                    &cpu->env, new_entry, perm, needed, true, false, false) :
                ia64_translation_exception_for_access(
                    &cpu->env, pte, key, perm, needed, true, false, false);
            if (pte_excp != IA64_EXCP_NONE) {
                if (probe) {
                    return false;
                }
                excp = pte_excp;
                goto raise_exception;
            }
            prot = ia64_tlb_prot_for_pte(&cpu->env,
                                         new_entry ? new_entry->pte : pte,
                                         perm, true);
            ia64_record_suppressed_tlb_fill_if_needed(
                &cpu->env, addr, mmu_idx,
                new_entry ? new_entry->pte : pte, perm, true, prot);
            qemu_log_mask(CPU_LOG_MMU,
                          "ia64 vhpt hit i va=0x%016" PRIx64
                          " rid=0x%06" PRIx32 " pa=0x%016" PRIx64
                          " perm=0x%x iha=0x%016" PRIx64 "\n",
                          (uint64_t)addr, rid, pa, perm,
                          ia64_vhpt_hash_address(&cpu->env, addr));
            ia64_tlb_set_entry_page(
                cs, addr, pa, page_size, prot, mmu_idx,
                ia64_pte_memory_speculation(new_entry ? new_entry->pte :
                                                        pte),
                ((new_entry ? new_entry->pte : pte) >> 2) & 7);
            return true;
        }
    }
    if (probe) {
        return false;
    }

    {
        uint64_t vhpt_entry_va;
        uint8_t vhpt_size;
        bool vhpt_long_format;
        bool vhpt_enabled = ia64_vhpt_walker_enabled(&cpu->env, addr,
                                                     is_ifetch, is_rse,
                                                     &vhpt_size,
                                                     &vhpt_long_format);

        if (!is_ifetch && ia64_data_nested_tlb_active(&cpu->env)) {
            excp = IA64_EXCP_DATA_NESTED_TLB;
        } else if (vhpt_enabled &&
                   ia64_vhpt_pte_not_present(&cpu->env, addr, is_ifetch,
                                             is_rse, &vhpt_entry_va)) {
            excp = IA64_EXCP_PAGE_NOT_PRESENT;
        } else if (!ia64_vhpt_entry_accessible(&cpu->env, addr, is_ifetch,
                                               is_rse, &vhpt_entry_va)) {
            excp = IA64_EXCP_VHPT_FAULT;
        } else if (vhpt_enabled) {
            excp = is_ifetch ? IA64_EXCP_ITLB_FAULT : IA64_EXCP_DTLB_FAULT;
        } else {
            excp = is_ifetch ? IA64_EXCP_ALT_ITLB : IA64_EXCP_ALT_DTLB;
        }
    }
raise_exception:
    if ((cpu->env.psr & IA64_PSR_IS) && retaddr) {
        cpu_restore_state(cs, retaddr);
        retaddr = 0;
    }
    if (cpu->env.psr & IA64_PSR_IS) {
        cpu->env.ip = ia64_ia32_virtual_ip(&cpu->env);
        cpu->env.exception_state.fault_ip = cpu->env.ip;
    }
    if (is_ifetch && excp == IA64_EXCP_PAGE_NOT_PRESENT &&
        (cpu->env.psr & IA64_PSR_IC) &&
        !(cpu->env.psr & IA64_PSR_IS)) {
        /*
         * IIP receives IP on interruption entry, and for faults it must point
         * at the faulting instruction bundle when interruption collection is
         * enabled.  Instruction fetch page-not-present faults may be raised
         * while looking up the next TB, before env->ip has otherwise advanced
         * to the fetched bundle.
         */
        cpu->env.ip = ia64_ip_bundle_addr(addr);
    }
    /*
     * IPSR.ri must name the slot execution resumes at.  PSR.ri holds
     * the current slot for data references and, for instruction
     * fetches, the slot the fetch will resume at (0 after a branch;
     * the interrupted slot when refetching after an rfi).  Without
     * this, an instruction-fetch fault would reuse a stale fault_slot
     * and the handler's rfi would skip slots of the target bundle.
     */
    cpu->env.exception_state.fault_slot =
        cpu->env.psr & IA64_PSR_IS ? 0 :
        (cpu->env.psr & IA64_PSR_RI_MASK) >> IA64_PSR_RI_SHIFT;
    if (cpu->env.psr & IA64_PSR_IC) {
        cpu->env.cr_ifa = is_ifetch && (cpu->env.psr & IA64_PSR_IS) ?
                          addr & ~0xfULL : addr;
        if (ia64_exception_initializes_iha(excp)) {
            cpu->env.cr_iha = ia64_vhpt_hash_address(&cpu->env, addr);
        }
        cpu->env.cr_itir = ia64_region_itir(
            &cpu->env, excp == IA64_EXCP_VHPT_FAULT ? cpu->env.cr_iha : addr);
    }
    if (excp != IA64_EXCP_DATA_NESTED_TLB) {
        if (excp == IA64_EXCP_UNIMPL_DATA_ADDR) {
            cpu->env.cr_isr = IA64_GENEX_UNIMPL_DATA_ADDR |
                              (access_type == MMU_DATA_STORE ?
                               IA64_ISR_W : IA64_ISR_R);
        } else if (excp == IA64_EXCP_UNIMPL_INST_ADDR) {
            cpu->env.cr_isr = IA64_GENEX_UNIMPL_INST_ADDR | IA64_ISR_X;
        } else {
            cpu->env.cr_isr = is_ifetch ? IA64_ISR_X :
                              (access_type == MMU_DATA_STORE ? IA64_ISR_W :
                               IA64_ISR_R);
            if (excp == IA64_EXCP_NAT_CONSUMPTION) {
                /*
                 * NaT Page Consumption reports ISR.code{5:4} = 2; the
                 * non-access code in ISR.code{3:0} is zero for an access.
                 */
                cpu->env.cr_isr |= IA64_ISR_CODE_NAT_PAGE;
            }
        }
        if (is_rse) {
            cpu->env.cr_isr |= IA64_ISR_RS;
            if (cpu->env.rse.rse_dirty < 0 || cpu->env.rse.rse_dirty_nat < 0) {
                /* Mandatory load for an incomplete frame (SDM 6.8). */
                cpu->env.cr_isr |= IA64_ISR_IR;
            }
        } else if (!is_ifetch && excp != IA64_EXCP_NAT_CONSUMPTION &&
                   ia64_current_code_tlb_ed(&cpu->env)) {
            /* NaT Page Consumption always reports ISR.ed as 0. */
            cpu->env.cr_isr |= IA64_ISR_ED;
        }
    }
    qemu_log_mask(CPU_LOG_MMU,
                  "ia64 tlb miss %c va=0x%016" PRIx64
                  " rid=0x%06" PRIx32 " ps=0x%016" PRIx64
                  " iha=0x%016" PRIx64 " pta=0x%016" PRIx64
                  " isr=0x%016" PRIx64 "\n",
                  is_ifetch ? 'i' :
                  (access_type == MMU_DATA_STORE ? 'w' : 'r'),
                  (uint64_t)addr, rid, cpu->env.cr_itir,
                  cpu->env.cr_iha, cpu->env.cr_pta, cpu->env.cr_isr);
    cs->exception_index = excp;
    if (cpu->env.psr & IA64_PSR_IS) {
        cpu_loop_exit(cs);
    }
    cpu_loop_exit_restore(cs, retaddr);
}


void ia64_cpu_set_boot_info(IA64CPU *cpu, const IA64BootInfo *info)
{
    cpu->boot_info = *info;
    cpu->boot_info_valid = true;
    cpu->boot_info_pending = true;
    CPU(cpu)->start_powered_off = info->powered_off;
}

void ia64_cpu_reset_to_boot_info(IA64CPU *cpu)
{
    g_assert(cpu->boot_info_valid);
    cpu->boot_info_pending = true;
    cpu_reset(CPU(cpu));
}

static void ia64_cpu_apply_boot_info(IA64CPU *cpu)
{
    CPUIA64State *env = &cpu->env;
    const IA64BootInfo *info = &cpu->boot_info;

    if (!cpu->boot_info_valid || !cpu->boot_info_pending) {
        return;
    }
    cpu->boot_info_pending = false;

    env->psr = 0;
    env->ip = info->firmware_entry;
    env->br[IA64_BR_RETURN_LINK] = info->firmware_entry;
    env->cr_iva = info->iva;
    /*
     * Start with the VHPT walker disabled and the architected minimum table
     * size.  A size smaller than 15 is a reserved PTA encoding.
     */
    env->cr_pta = 15ULL << IA64_PTA_SIZE_SHIFT;
    env->cr_dcr = IA64_DCR_DM | IA64_DCR_DP;
    env->ar_kr0 =
        ia64_cpu_default_io_block_pa(IA64_CPU_GET_CLASS(cpu));
    env->ar_kr7 = 0;
    env->ar_rsc = info->rsc;
    env->ar_bsp = info->bsp;
    env->ar_bspstore = info->bsp;
    env->ar_rnat = 0;
    ia64_rse_rnat_undefined(env, "reset");
    env->gr[IA64_GR_STACK_POINTER] = info->stack_pointer;
    env->gr[IA64_GR_GLOBAL_POINTER] = info->global_pointer;
    env->interrupt.pal_halt_wake = info->powered_off;
    env->ar_fpsr = IA64_FPSR_DEFAULT;
    set_float_rounding_mode(float_round_nearest_even, &env->fp.fp_status);
    set_flush_to_zero(false, &env->fp.fp_status);
    set_flush_inputs_to_zero(false, &env->fp.fp_status);
    set_default_nan_mode(false, &env->fp.fp_status);
}

static void ia64_cpu_reset_hold(Object *obj, ResetType type)
{
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(obj);
    IA64CPU *cpu = IA64_CPU(obj);

    if (icc->parent_phases.hold) {
        icc->parent_phases.hold(obj, type);
    }

    if (cpu->itm_timer != NULL) {
        timer_del(cpu->itm_timer);
    }
    memset(&cpu->env, 0, sizeof(cpu->env));
    cpu->env.alat_state.alat_full = cpu->alat_full;
    cpu->env.fp.fr[IA64_FR_ONE_INDEX] = IA64_FR_ONE;
    cpu->env.pr[IA64_PR_TRUE] = 1;
    cpu->env.psr = 0;
    cpu->env.ar_rsc = 0;
    /* Empty frame: every stacked physical register is invalid. */
    cpu->env.rse.rse_invalid = IA64_STACKED_GR_COUNT;
    cpu->env.ar_fpsr = IA64_FPSR_DEFAULT;
    cpu->env.cr_iva = 0;
    cpu->env.instruction_group_start = true;
    ia64_itc_write(&cpu->env, 0);
    set_float_2nan_prop_rule(float_2nan_prop_ab, &cpu->env.fp.fp_status);
    set_float_3nan_prop_rule(float_3nan_prop_abc, &cpu->env.fp.fp_status);
    set_float_infzeronan_rule(float_infzeronan_dnan_never,
                              &cpu->env.fp.fp_status);
    set_float_default_nan_pattern(0b01000000, &cpu->env.fp.fp_status);
    cpu->env.cr[IA64_CR_SAPIC_LID] =
        ia64_sapic_lid(MAX(CPU(cpu)->cpu_index, 0), 0);
    cpu->env.cr[IA64_CR_SAPIC_TPR] = 0;
    cpu->env.cr[IA64_CR_ITV] = IA64_VECTOR_MASKED;
    if (icc->model == IA64_CPU_MODEL_MERCED) {
        cpu->env.pmc[8] = 0xf00000003ffffff8ULL;
        cpu->env.pmc[9] = 0xf00000003ffffff8ULL;
        cpu->env.pmc[11] = 1ULL << 28;
        cpu->env.pmc[13] = 1;
    }
    cpu->env.pal.pal_bus_feature_status = 0;
    cpu->env.pal.pal_proc_feature_status =
        icc->model == IA64_CPU_MODEL_MONTECITO ?
        (PAL_PROC_MONTECITO_ICACHE_COHERENCE |
         PAL_PROC_MONTECITO_EXCLUSIVE_PREFETCH |
         PAL_PROC_MONTECITO_HT) : 0;
    cpu->env.pal.pal_proc_copy_valid = false;
    cpu->env.pal.pal_proc_copy_addr = 0;
    cpu->env.pal.pal_interrupt_block_addr = IA64_LOCAL_SAPIC_PA;
    cpu->env.pal.pal_io_block_addr =
        ia64_cpu_default_io_block_pa(icc);
    ia64_cpu_apply_boot_info(cpu);
}

static ObjectClass *ia64_cpu_class_by_name(const char *cpu_model)
{
    char *typename;
    ObjectClass *oc;

    typename = g_strdup_printf(IA64_CPU_TYPE_NAME("%s"), cpu_model);
    oc = object_class_by_name(typename);
    g_free(typename);
    return oc;
}

static void ia64_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    IA64CPU *cpu = IA64_CPU(dev);
    IA64CPUClass *icc = IA64_CPU_GET_CLASS(dev);
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    cpu->itm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ia64_itm_timer_cb, cpu);

    qemu_init_vcpu(cs);
    cpu_reset(cs);

    icc->parent_realize(dev, errp);
}

static const struct SysemuCPUOps ia64_sysemu_ops = {
    .has_work = ia64_cpu_has_work,
    .get_phys_page_debug = ia64_cpu_get_phys_page_debug,
};

static const TCGCPUOps ia64_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    .mttcg_supported = true,
    .precise_smc = true,
    .initialize = ia64_translate_init,
    .translate_code = ia64_translate_code,
    .get_tb_cpu_state = ia64_get_tb_cpu_state,
    .synchronize_from_tb = ia64_cpu_synchronize_from_tb,
    .restore_state_to_opc = ia64_restore_state_to_opc,
    .mmu_index = ia64_cpu_mmu_index,
    .tlb_fill = ia64_cpu_tlb_fill,
    .pointer_wrap = ia64_pointer_wrap,
#ifndef CONFIG_USER_ONLY
    .do_unaligned_access = ia64_cpu_do_unaligned_access,
#endif
    .cpu_exec_interrupt = ia64_cpu_exec_interrupt,
    .cpu_exec_halt = ia64_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = ia64_cpu_do_interrupt,
};

#define IA64_ITANIUM2_MEMORY_ATTRIBUTE_MASK \
    ((1U << IA64_PTE_MA_WB) | (1U << IA64_PTE_MA_UC) | \
     (1U << IA64_PTE_MA_UCE) | (1U << IA64_PTE_MA_WC))

#define IA64_FREQUENCY_RATIO(numerator, denominator) \
    (((uint64_t)(numerator) << 32) | (denominator))

static void ia64_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    IA64CPUClass *icc = IA64_CPU_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    device_class_set_parent_realize(dc, ia64_cpu_realize,
                                    &icc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, ia64_cpu_reset_hold, NULL,
                                       &icc->parent_phases);

    cc->class_by_name = ia64_cpu_class_by_name;
    cc->dump_state = ia64_cpu_dump_state;
    cc->set_pc = ia64_cpu_set_pc;
    cc->get_pc = ia64_cpu_get_pc;
    cc->sysemu_ops = &ia64_sysemu_ops;
    cc->gdb_read_register = ia64_cpu_gdb_read_register;
    cc->gdb_write_register = ia64_cpu_gdb_write_register;
    cc->gdb_num_core_regs = IA64_GDB_NUM_CORE_REGS;
    cc->tcg_ops = &ia64_tcg_ops;
    dc->vmsd = &vmstate_ia64_cpu;

    icc->model = IA64_CPU_MODEL_MONTECITO;
    icc->cpuid_version = 0x0000000020000704ULL;
    icc->cpuid_features = IA64_CPUID4_LB | IA64_CPUID4_AO;
    icc->pal_version = 0x0000096801000968ULL;
    icc->frequency_base_hz = 100000000;
    icc->itc_frequency_hz = 400000000;
    icc->processor_frequency_ratio = IA64_FREQUENCY_RATIO(16, 1);
    icc->bus_frequency_ratio = IA64_FREQUENCY_RATIO(16, 3);
    icc->itc_frequency_ratio = IA64_FREQUENCY_RATIO(4, 1);
    icc->ia32_cpuid_version = 0;
    memset(icc->ia32_cpuid_leaf2, 0, sizeof(icc->ia32_cpuid_leaf2));
    icc->insertable_page_size_mask = IA64_INSERTABLE_PAGE_SIZE_MASK;
    icc->purgeable_page_size_mask = IA64_PURGEABLE_PAGE_SIZE_MASK;
    icc->itr_count = 32;
    icc->dtr_count = 32;
    icc->itlb_entries = IA64_TLB_MAX;
    icc->dtlb_entries = IA64_TLB_MAX;
    icc->phys_addr_bits = IA64_IMPL_PA_BITS;
    icc->impl_va_msb = IA64_IMPL_VA_MSB;
    icc->rid_bits = IA64_IMPL_RID_BITS;
    icc->key_bits = IA64_IMPL_KEY_BITS;
    icc->hash_tag_id = 8;
    icc->unique_tcs = 4;
    icc->tc_levels = 2;
    icc->perf_counter_width = 48;
    /*
     * The Madison model implements WB, UC, UCE, and WC.  TCG has no
     * physical write-coalescing buffer, so WC does not promise physical
     * coalescing or timing; its architected access, ordering, speculation,
     * and unsupported-operation rules remain distinct from WB.
     */
    icc->memory_attribute_mask = IA64_ITANIUM2_MEMORY_ATTRIBUTE_MASK;
    icc->implemented_pmc_mask = 0x3fffULL;
    icc->implemented_pmd_mask = 0x3ffffULL;
    icc->perf_cycles_mask = 0xf0ULL;
    icc->perf_retired_mask = 0xf0ULL;
    icc->rse_has_clean_partition = true;
    icc->has_native_ia32 = false;
    icc->has_virtualization = true;
    icc->is_montecito = true;
}

typedef struct IA64CPUModelDef {
    IA64CPUModel model;
    uint64_t cpuid_version;
    uint64_t cpuid_features;
    uint64_t pal_version;
    uint32_t frequency_base_hz;
    uint32_t itc_frequency_hz;
    uint64_t processor_frequency_ratio;
    uint64_t bus_frequency_ratio;
    uint64_t itc_frequency_ratio;
    uint32_t ia32_cpuid_version;
    uint32_t ia32_cpuid_leaf2[4];
    uint64_t insertable_page_size_mask;
    uint64_t purgeable_page_size_mask;
    uint8_t itr_count;
    uint8_t dtr_count;
    uint16_t itlb_entries;
    uint16_t dtlb_entries;
    uint8_t phys_addr_bits;
    uint8_t impl_va_msb;
    uint8_t rid_bits;
    uint8_t key_bits;
    uint8_t hash_tag_id;
    uint8_t unique_tcs;
    uint8_t tc_levels;
    uint8_t perf_counter_width;
    uint8_t memory_attribute_mask;
    uint64_t implemented_pmc_mask;
    uint64_t implemented_pmd_mask;
    uint64_t perf_cycles_mask;
    uint64_t perf_retired_mask;
    bool rse_has_clean_partition;
    bool has_native_ia32;
    bool has_virtualization;
    bool is_montecito;
} IA64CPUModelDef;

static void ia64_cpu_model_class_init(ObjectClass *oc, const void *data)
{
    IA64CPUClass *icc = IA64_CPU_CLASS(oc);
    const IA64CPUModelDef *model = data;

    icc->model = model->model;
    icc->cpuid_version = model->cpuid_version;
    icc->cpuid_features = model->cpuid_features;
    icc->pal_version = model->pal_version;
    icc->frequency_base_hz = model->frequency_base_hz;
    icc->itc_frequency_hz = model->itc_frequency_hz;
    icc->processor_frequency_ratio = model->processor_frequency_ratio;
    icc->bus_frequency_ratio = model->bus_frequency_ratio;
    icc->itc_frequency_ratio = model->itc_frequency_ratio;
    icc->ia32_cpuid_version = model->ia32_cpuid_version;
    memcpy(icc->ia32_cpuid_leaf2, model->ia32_cpuid_leaf2,
           sizeof(icc->ia32_cpuid_leaf2));
    icc->insertable_page_size_mask = model->insertable_page_size_mask;
    icc->purgeable_page_size_mask = model->purgeable_page_size_mask;
    icc->itr_count = model->itr_count;
    icc->dtr_count = model->dtr_count;
    icc->itlb_entries = model->itlb_entries;
    icc->dtlb_entries = model->dtlb_entries;
    icc->phys_addr_bits = model->phys_addr_bits;
    icc->impl_va_msb = model->impl_va_msb;
    icc->rid_bits = model->rid_bits;
    icc->key_bits = model->key_bits;
    icc->hash_tag_id = model->hash_tag_id;
    icc->unique_tcs = model->unique_tcs;
    icc->tc_levels = model->tc_levels;
    icc->perf_counter_width = model->perf_counter_width;
    icc->memory_attribute_mask = model->memory_attribute_mask;
    icc->implemented_pmc_mask = model->implemented_pmc_mask;
    icc->implemented_pmd_mask = model->implemented_pmd_mask;
    icc->perf_cycles_mask = model->perf_cycles_mask;
    icc->perf_retired_mask = model->perf_retired_mask;
    icc->rse_has_clean_partition = model->rse_has_clean_partition;
    icc->has_native_ia32 = model->has_native_ia32;
    icc->has_virtualization = model->has_virtualization;
    icc->is_montecito = model->is_montecito;

    g_assert(model->itlb_entries > 0 &&
             model->itlb_entries <= IA64_TLB_MAX);
    g_assert(model->dtlb_entries > 0 &&
             model->dtlb_entries <= IA64_TLB_MAX);
    g_assert(model->itr_count <= model->itlb_entries);
    g_assert(model->dtr_count <= model->dtlb_entries);
    g_assert(model->frequency_base_hz > 0);
    g_assert(model->itc_frequency_hz > 0 &&
             model->itc_frequency_hz <= 1600000000);
    g_assert((uint32_t)model->itc_frequency_ratio != 0);
    g_assert((uint64_t)model->frequency_base_hz *
             (model->itc_frequency_ratio >> 32) /
             (uint32_t)model->itc_frequency_ratio ==
             model->itc_frequency_hz);
}

static const IA64CPUModelDef ia64_cpu_model_merced = {
    .model = IA64_CPU_MODEL_MERCED,
    .cpuid_version = 0x0000000007000804ULL,
    .cpuid_features = 0,
    /*
     * PAL_A model 8 and PAL_B model 8 use revision 30 for this release.
     */
    .pal_version = 0x0000883001008830ULL,
    /*
     * The selected 800 MHz part advances ITC at the processor rate.  Keep
     * this ratio explicit because the dual-core model uses a divided ITC.
     */
    .frequency_base_hz = 100000000,
    .itc_frequency_hz = 800000000,
    .processor_frequency_ratio = IA64_FREQUENCY_RATIO(8, 1),
    .bus_frequency_ratio = IA64_FREQUENCY_RATIO(4, 3),
    .itc_frequency_ratio = IA64_FREQUENCY_RATIO(8, 1),
    /*
     * Public product documentation specifies family 7 and the cache/TLB
     * descriptors but does not publish the complete native IA-32 leaf-1
     * signature.  Family 7, model 1, stepping 5 is retained as this model's
     * compatibility identity; it is not used to select execution behavior.
     */
    .ia32_cpuid_version = 0x00000715,
    .ia32_cpuid_leaf2 = {
        0x00151001, 0x0000891a, 0x009b9690, 0x80000000,
    },
    /*
     * The architectural page-size table requires 64 MiB support on every
     * processor, while the product-specific summary omits that size without
     * declaring an exception.  Follow the normative architectural table.
     */
    .insertable_page_size_mask =
        (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | (1ULL << 16) |
        (1ULL << 18) | (1ULL << 20) | (1ULL << 22) | (1ULL << 24) |
        (1ULL << 26) | (1ULL << 28),
    .purgeable_page_size_mask =
        (1ULL << 12) | (1ULL << 13) | (1ULL << 14) | (1ULL << 16) |
        (1ULL << 18) | (1ULL << 20) | (1ULL << 22) | (1ULL << 24) |
        (1ULL << 26) | (1ULL << 28) | (1ULL << 32),
    .itr_count = 8,
    .dtr_count = 48,
    /*
     * The architecturally visible translation storage is the 64-entry
     * instruction TLB and the 96-entry main data TLB.  The 32-entry
     * first-level data TLB only caches the main data TLB and is not extra
     * TR/TC storage.
     */
    .itlb_entries = 64,
    .dtlb_entries = 96,
    .phys_addr_bits = 44,
    .impl_va_msb = 50,
    .rid_bits = 18,
    .key_bits = 21,
    .hash_tag_id = 0,
    .unique_tcs = 3,
    .tc_levels = 2,
    .perf_counter_width = 32,
    .memory_attribute_mask = (1U << IA64_PTE_MA_WB) |
                             (1U << IA64_PTE_MA_UC) |
                             (1U << IA64_PTE_MA_WC) |
                             (1U << IA64_PTE_MA_NATPAGE),
    .implemented_pmc_mask = 0x3fffULL,
    .implemented_pmd_mask = 0x3ffffULL,
    .perf_cycles_mask = 0xf0ULL,
    /*
     * The product manual records the old-PAL 0x10 result.  The processor
     * update identifies that as an erratum through PAL 7.7.28; PAL 8.8.30,
     * advertised above, reports the implemented PMC4/PMC5 pair.
     */
    .perf_retired_mask = 0x30ULL,
    .rse_has_clean_partition = false,
    .has_native_ia32 = true,
};

static const IA64CPUModelDef ia64_cpu_model_madison = {
    .model = IA64_CPU_MODEL_MADISON,
    /* Family 0x1f, model 1, revision 5, CPUID[4] is the last register. */
    .cpuid_version = 0x000000001f010504ULL,
    /*
     * The selected family 0x1f/model 1/revision 5 processor reports only
     * long-branch support in CPUID[4].  In particular, early deferral
     * selected through PAL is not the CPUID spontaneous-deferral feature.
     */
    .cpuid_features = IA64_CPUID4_LB,
    /* Latest documented PAL release for the selected B1 model. */
    .pal_version = 0x0000057301000573ULL,
    .frequency_base_hz = 100000000,
    .itc_frequency_hz = 1600000000,
    .processor_frequency_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_frequency_ratio = IA64_FREQUENCY_RATIO(4, 1),
    .itc_frequency_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .ia32_cpuid_version = 0x00000673,
    .ia32_cpuid_leaf2 = {
        0x7e776701, 0x0000008d, 0, 0x80000000,
    },
    .insertable_page_size_mask = IA64_INSERTABLE_PAGE_SIZE_MASK,
    .purgeable_page_size_mask = IA64_PURGEABLE_PAGE_SIZE_MASK,
    .itr_count = 64,
    .dtr_count = 64,
    .itlb_entries = IA64_TLB_MAX,
    .dtlb_entries = IA64_TLB_MAX,
    .phys_addr_bits = IA64_IMPL_PA_BITS,
    .impl_va_msb = IA64_IMPL_VA_MSB,
    .rid_bits = IA64_IMPL_RID_BITS,
    .key_bits = IA64_IMPL_KEY_BITS,
    .hash_tag_id = 8,
    .unique_tcs = 4,
    .tc_levels = 2,
    .perf_counter_width = 48,
    .memory_attribute_mask = IA64_ITANIUM2_MEMORY_ATTRIBUTE_MASK,
    .implemented_pmc_mask = 0x3fffULL,
    .implemented_pmd_mask = 0x3ffffULL,
    .perf_cycles_mask = 0xf0ULL,
    .perf_retired_mask = 0xf0ULL,
    .rse_has_clean_partition = true,
    .has_native_ia32 = true,
    .has_virtualization = false,
};

static const IA64CPUModelDef ia64_cpu_model_montecito = {
    .model = IA64_CPU_MODEL_MONTECITO,
    /* Family 0x20, model 0, C2 revision 7, CPUID[4] is the last register. */
    .cpuid_version = 0x0000000020000704ULL,
    /* C2 reports long-branch and 16-byte atomic support (CPUID[4] = 5). */
    .cpuid_features = IA64_CPUID4_LB | IA64_CPUID4_AO,
    /* Latest documented PAL release for the selected C2 model. */
    .pal_version = 0x0000096801000968ULL,
    /*
     * This model's ITC is one quarter of its 1.6 GHz processor clock.
     * Advertising a 4:1 ratio while advancing a different host-side rate
     * makes operating-system timer calibration internally inconsistent.
     */
    .frequency_base_hz = 100000000,
    .itc_frequency_hz = 400000000,
    .processor_frequency_ratio = IA64_FREQUENCY_RATIO(16, 1),
    .bus_frequency_ratio = IA64_FREQUENCY_RATIO(16, 3),
    .itc_frequency_ratio = IA64_FREQUENCY_RATIO(4, 1),
    .ia32_cpuid_version = 0,
    .ia32_cpuid_leaf2 = { 0, 0, 0, 0 },
    .insertable_page_size_mask = IA64_INSERTABLE_PAGE_SIZE_MASK,
    .purgeable_page_size_mask = IA64_PURGEABLE_PAGE_SIZE_MASK,
    .itr_count = 32,
    .dtr_count = 32,
    .itlb_entries = IA64_TLB_MAX,
    .dtlb_entries = IA64_TLB_MAX,
    .phys_addr_bits = IA64_IMPL_PA_BITS,
    .impl_va_msb = IA64_IMPL_VA_MSB,
    .rid_bits = IA64_IMPL_RID_BITS,
    .key_bits = IA64_IMPL_KEY_BITS,
    .hash_tag_id = 8,
    .unique_tcs = 4,
    .tc_levels = 2,
    .perf_counter_width = 48,
    /*
     * The dual-core update inherits the Madison WB/UC/UCE/WC memory
     * attributes and explicitly gives UC/UCE/WC restrictions for its new
     * 16-byte operations.
     */
    .memory_attribute_mask = IA64_ITANIUM2_MEMORY_ATTRIBUTE_MASK,
    .implemented_pmc_mask = 0x3fffULL,
    .implemented_pmd_mask = 0x3ffffULL,
    .perf_cycles_mask = 0xf0ULL,
    .perf_retired_mask = 0xf0ULL,
    .rse_has_clean_partition = true,
    /*
     * Montecito removed native IA-32 hardware.  Product documentation
     * describes a closed PAL-based translation layer for pre-OS use after
     * PAL_COPY_PAL, but does not publish the translator implementation; the
     * copied QEMU PAL image is only a procedure-call portal.  Do not treat
     * that copy as enabling native PSR.is transitions, because doing so would
     * also expose unsupported PAL-based execution to an operating system.
     */
    .has_native_ia32 = false,
    /*
     * Montecito implements the virtualization extensions, but this model
     * does not virtualize.  vmsw is decoded and reported as a Virtualization
     * fault so a guest sees the architected interruption instead of a
     * silently succeeding privilege-mode switch.
     */
    .has_virtualization = true,
    .is_montecito = true,
};

static const TypeInfo ia64_cpu_type_info[] = {
    {
        .name = TYPE_IA64_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(IA64CPU),
        .instance_align = __alignof__(IA64CPU),
        .class_size = sizeof(IA64CPUClass),
        .class_init = ia64_cpu_class_init,
        .abstract = true,
    },
    {
        .name = IA64_CPU_TYPE_NAME("merced"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_merced,
    },
    {
        .name = IA64_CPU_TYPE_NAME("madison"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_madison,
    },
    {
        .name = IA64_CPU_TYPE_NAME("montecito"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_montecito,
    },
    {
        .name = IA64_CPU_TYPE_NAME("itanium"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_merced,
    },
    {
        .name = IA64_CPU_TYPE_NAME("itanium2"),
        .parent = TYPE_IA64_CPU,
        .class_init = ia64_cpu_model_class_init,
        .class_data = &ia64_cpu_model_montecito,
    },
};

DEFINE_TYPES(ia64_cpu_type_info)
