/* Adapt the shared memory helpers to IA-32 state. */

#ifndef TARGET_IA64_IA32_MEM_HELPER_ADAPTER_H
#define TARGET_IA64_IA32_MEM_HELPER_ADAPTER_H

#include "helper-compat.h"
#define X86_CPU_ARCH_ENV(env) ((CPUIA64State *)(env))

#endif /* TARGET_IA64_IA32_MEM_HELPER_ADAPTER_H */
