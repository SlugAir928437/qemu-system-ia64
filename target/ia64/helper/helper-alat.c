/* IA-64 TCG helper ABI adapters for the Advanced Load Address Table. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"

uint64_t helper_chk_a(CPUIA64State *env, uint64_t va, uint32_t reg)
{
    return ia64_alat_chk_a(env, va, reg);
}

void helper_invala(CPUIA64State *env)
{
    ia64_alat_invala(env);
}

void helper_set_alat(CPUIA64State *env, uint32_t reg, uint64_t addr,
                     uint32_t size)
{
    ia64_alat_set(env, reg, addr, size);
}

void helper_set_alat_fp(CPUIA64State *env, uint32_t reg, uint64_t addr,
                        uint32_t size)
{
    ia64_alat_set_fp(env, reg, addr, size);
}

void helper_invalidate_alat_reg(CPUIA64State *env, uint32_t reg)
{
    ia64_alat_invalidate_reg(env, reg);
}

void helper_invalidate_alat_fp_reg(CPUIA64State *env, uint32_t reg)
{
    ia64_alat_invalidate_fp_reg(env, reg);
}

uint64_t helper_check_load_alat(CPUIA64State *env, uint32_t reg,
                                uint32_t clear)
{
    return ia64_alat_check_load(env, reg, clear);
}

uint64_t helper_check_load_alat_addr(CPUIA64State *env, uint32_t reg,
                                     uint64_t addr, uint32_t size,
                                     uint32_t clear)
{
    return ia64_alat_check_load_addr(env, reg, addr, size, clear);
}

uint64_t helper_check_load_alat_fp(CPUIA64State *env, uint32_t reg,
                                   uint32_t clear)
{
    return ia64_alat_check_load_fp(env, reg, clear);
}

uint64_t helper_check_load_alat_fp_addr(CPUIA64State *env, uint32_t reg,
                                        uint64_t addr, uint32_t size,
                                        uint32_t clear)
{
    return ia64_alat_check_load_fp_addr(env, reg, addr, size, clear);
}

void helper_invalidate_alat_store(CPUIA64State *env, uint64_t addr,
                                  uint32_t size)
{
    ia64_invalidate_alat_store(env, addr, size);
}
