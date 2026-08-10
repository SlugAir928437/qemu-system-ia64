/* Adapt the shared floating-point and vector helpers to IA-32 state. */

#ifndef TARGET_IA64_IA32_FPU_HELPER_ADAPTER_H
#define TARGET_IA64_IA32_FPU_HELPER_ADAPTER_H

#include "helper-compat.h"
#define X86_CPU_ARCH_ENV(env) ((CPUIA64State *)(env))
#define X86_MASKMOV_ACCESS(env, addr, ra)                              \
    ia64_ia32_check_segment_access(                                   \
        (env), (uint32_t)(addr), R_DS, 1,                             \
        IA64_IA32_SEG_ACCESS_WRITE, (ra))
#define X86_FPU_ALWAYS_NE 1
#define X86_MXCSR_VALID_MASK 0x0000ffbfU
#include "ia32/ia32.h"

#endif /* TARGET_IA64_IA32_FPU_HELPER_ADAPTER_H */
