/* IA-64 TCG helper ABI adapters for atomic memory operations. */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "arch/arch.h"

uint64_t helper_cmpxchg(CPUIA64State *env, uint64_t addr, uint64_t cmp,
                        uint64_t val, uint32_t size)
{
    return ia64_memory_cmpxchg(env, addr, cmp, val, size, GETPC());
}

uint64_t helper_cmp8xchg16(CPUIA64State *env, uint64_t addr, uint64_t cmp,
                           uint64_t val, uint64_t csd)
{
    return ia64_memory_cmp8xchg16(env, addr, cmp, val, csd, GETPC());
}
