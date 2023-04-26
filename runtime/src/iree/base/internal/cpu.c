// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// NOTE: must be first before _any_ system includes.
#define _GNU_SOURCE

#include "iree/base/internal/cpu.h"

#include "iree/base/target_platform.h"
#include "iree/base/tracing.h"
#include "iree/schemas/cpu_data.h"

//===----------------------------------------------------------------------===//
// Platform-specific processor data queries
//===----------------------------------------------------------------------===//

#define IREE_COPY_BITS(dst_val, dst_mask, src_val, src_mask) \
  ((dst_val) |= (iree_all_bits_set((src_val), (src_mask)) ? (dst_mask) : 0))

#if defined(IREE_ARCH_ARM_64)
// On ARM, CPU feature info is not directly accessible to userspace (EL0). The
// OS needs to be involved one way or another.

#if defined(IREE_PLATFORM_ANDROID) || defined(IREE_PLATFORM_LINUX)

// For now as we only need ISA feature bits and no CPU identification beyond
// that, and as we are OK with requiring a sufficiently recent linux kernel to
// expose the features that we need, we can just rely on the basic HWCAP way.
#include <sys/auxv.h>

// NOTE: not all kernel versions have all of the cap bits we need defined so as
// a practice we always define the feature bits we need locally.
// https://docs.kernel.org/arm64/elf_hwcaps.html
#define IREE_HWCAP_ASIMDDP (1u << 20)
#define IREE_HWCAP2_I8MM (1u << 13)

static void iree_cpu_initialize_from_platform_arm_64(uint64_t* out_fields) {
  uint32_t hwcap = getauxval(AT_HWCAP);
  uint32_t hwcap2 = getauxval(AT_HWCAP2);
  uint64_t out0 = 0;
  IREE_COPY_BITS(out0, IREE_CPU_DATA0_ARM_64_DOTPROD, hwcap,
                 IREE_HWCAP_ASIMDDP);
  IREE_COPY_BITS(out0, IREE_CPU_DATA0_ARM_64_I8MM, hwcap2, IREE_HWCAP2_I8MM);
  out_fields[0] = out0;
}

#elif defined(IREE_PLATFORM_MACOS) || defined(IREE_PLATFORM_IOS)

#include <sys/sysctl.h>
#include <sys/types.h>

#define IREE_QUERY_SYSCTL(key, field_value, field_bit)            \
  do {                                                            \
    int64_t result = 0;                                           \
    size_t result_size = sizeof result;                           \
    if (0 == sysctlbyname(key, &result, &result_size, NULL, 0)) { \
      if (result) field_value |= field_bit;                       \
    }                                                             \
  } while (0)

static void iree_cpu_initialize_from_platform_arm_64(uint64_t* out_fields) {
  IREE_QUERY_SYSCTL("hw.optional.arm.FEAT_DotProd", out_fields[0],
                    IREE_CPU_DATA0_ARM_64_DOTPROD);
  IREE_QUERY_SYSCTL("hw.optional.arm.FEAT_I8MM", out_fields[0],
                    IREE_CPU_DATA0_ARM_64_I8MM);
}

#else

static void iree_cpu_initialize_from_platform_arm_64(uint64_t* out_fields) {
  // No implementation available. CPU data will be all zeros.
}

#endif  // IREE_PLATFORM_*

#elif defined(IREE_ARCH_X86_64)

#if defined(__GNUC__)
#include <cpuid.h>
#elif defined(_MSC_VER)
#include <intrin.h>
#endif

typedef struct iree_cpuid_regs_t {
  uint32_t eax;
  uint32_t ebx;
  uint32_t ecx;
  uint32_t edx;
} iree_cpuid_regs_t;

static inline iree_cpuid_regs_t iree_cpuid_raw(uint32_t eax, uint32_t ecx) {
  iree_cpuid_regs_t regs;
#if defined(__GNUC__)
  __cpuid_count(eax, ecx, regs.eax, regs.ebx, regs.ecx, regs.edx);
#elif defined(_MSC_VER)
  int regs_array[4];
  __cpuidex(regs_array, (int)eax, (int)ecx);
  regs.eax = regs_array[0];
  regs.ebx = regs_array[1];
  regs.ecx = regs_array[2];
  regs.edx = regs_array[3];
#else
#error What's the __cpuidex built-in for this compiler?
#endif
  return regs;
}

typedef struct iree_cpuid_bounds_t {
  uint32_t max_base_eax;
  uint32_t max_extended_eax;
} iree_cpuid_bounds_t;

static inline iree_cpuid_bounds_t iree_cpuid_query_bounds() {
  iree_cpuid_bounds_t bounds;
  bounds.max_base_eax = iree_cpuid_raw(0, 0).eax;
  bounds.max_extended_eax = iree_cpuid_raw(0x80000000u, 0).eax;
  if (bounds.max_extended_eax < 0x80000000u) bounds.max_extended_eax = 0;
  return bounds;
}

static inline bool iree_cpuid_is_in_range(uint32_t eax, uint32_t ecx,
                                          iree_cpuid_bounds_t bounds) {
  if (eax < 0x80000000u) {
    // EAX is a base function id.
    if (eax > bounds.max_base_eax) return false;
  } else {
    // EAX is an extended function id.
    if (eax > bounds.max_extended_eax) return false;
  }
  if (ecx) {
    // ECX is a nonzero sub-function id.
    uint32_t max_ecx = iree_cpuid_raw(eax, 0).eax;
    if (ecx > max_ecx) return false;
  }
  return true;
}

static inline iree_cpuid_regs_t iree_cpuid_or_zero(uint32_t eax, uint32_t ecx,
                                                   iree_cpuid_bounds_t bounds) {
  if (!iree_cpuid_is_in_range(eax, ecx, bounds)) {
    return (iree_cpuid_regs_t){0, 0, 0, 0};
  }
  return iree_cpuid_raw(eax, ecx);
}

static void iree_cpu_initialize_from_platform_x86_64(uint64_t* out_fields) {
  iree_cpuid_bounds_t bounds = iree_cpuid_query_bounds();
  iree_cpuid_regs_t leaf1 = iree_cpuid_or_zero(1, 0, bounds);
  iree_cpuid_regs_t leaf7_0 = iree_cpuid_or_zero(7, 0, bounds);
  iree_cpuid_regs_t leaf7_1 = iree_cpuid_or_zero(7, 1, bounds);
  iree_cpuid_regs_t leafD = iree_cpuid_or_zero(0xD, 0, bounds);
  iree_cpuid_regs_t leafExt1 = iree_cpuid_or_zero(0x80000001u, 0, bounds);

  // Bits are given by bit position not by hex value because this is how they
  // are described in the Intel Architectures Software Developer's Manual,
  // Table 3-8, "Information Returned by CPUID Instruction".

  uint64_t out0 = 0;
  IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_SSE3, leaf1.ecx, 1 << 0);
  IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_SSSE3, leaf1.ecx, 1 << 9);
  IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_SSE41, leaf1.ecx, 1 << 19);
  IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_SSE42, leaf1.ecx, 1 << 20);
  IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_SSE4A, leafExt1.ecx, 1 << 6);

  // Features that depend on YMM registers being enabled by the OS.
  if (iree_all_bits_set(leafD.eax, 0x7)) {
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX, leaf1.ecx, 1 << 28);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_FMA, leaf1.ecx, 1 << 12);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_FMA4, leafExt1.ecx, 1 << 16);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_XOP, leafExt1.ecx, 1 << 11);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_F16C, leaf1.ecx, 1 << 29);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX2, leaf7_0.ebx, 1 << 5);
  }

  // Features that depend on ZMM registers being enabled by the OS.
  if (iree_all_bits_set(leafD.eax, 0xE7)) {
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512F, leaf7_0.ebx, 1 << 16);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512CD, leaf7_0.ebx, 1 << 28);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512VL, leaf7_0.ebx, 1u << 31);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512DQ, leaf7_0.ebx, 1 << 17);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512BW, leaf7_0.ebx, 1 << 30);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512IFMA, leaf7_0.ebx,
                   1 << 21);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512VBMI, leaf7_0.ecx, 1 << 1);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512VPOPCNTDQ, leaf7_0.ecx,
                   1 << 14);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512VNNI, leaf7_0.ecx,
                   1 << 11);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512VBMI2, leaf7_0.ecx,
                   1 << 6);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512BITALG, leaf7_0.ecx,
                   1 << 12);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512BF16, leaf7_1.eax, 1 << 5);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AVX512FP16, leaf7_0.edx,
                   1 << 23);
  }

  // Features that depend on AMX TILE state being enabled by the OS.
  if (iree_all_bits_set(leafD.eax, 0x60000)) {
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AMXTILE, leaf7_0.edx, 1 << 24);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AMXINT8, leaf7_0.edx, 1 << 25);
    IREE_COPY_BITS(out0, IREE_CPU_DATA0_X86_64_AMXBF16, leaf7_0.edx, 1 << 22);
  }

  out_fields[0] = out0;
}

#endif  // defined(IREE_ARCH_ARM_64)

static void iree_cpu_initialize_from_platform(iree_allocator_t temp_allocator,
                                              uint64_t* out_fields) {
#if defined(IREE_ARCH_ARM_64)
  iree_cpu_initialize_from_platform_arm_64(out_fields);
#elif defined(IREE_ARCH_X86_64)
  iree_cpu_initialize_from_platform_x86_64(out_fields);
#else
  // No implementation available. CPU data will be all zeros.
#endif  // defined(IREE_ARCH_ARM_64)
}

//===----------------------------------------------------------------------===//
// Processor data query
//===----------------------------------------------------------------------===//

static iree_alignas(64) uint64_t
    iree_cpu_data_cache_[IREE_CPU_DATA_FIELD_COUNT] = {0};

void iree_cpu_initialize(iree_allocator_t temp_allocator) {
  IREE_TRACE_ZONE_BEGIN(z0);
  memset(iree_cpu_data_cache_, 0, sizeof(iree_cpu_data_cache_));
  iree_cpu_initialize_from_platform(temp_allocator, iree_cpu_data_cache_);
  IREE_TRACE_ZONE_END(z0);
}

void iree_cpu_initialize_with_data(iree_host_size_t field_count,
                                   const uint64_t* fields) {
  memset(iree_cpu_data_cache_, 0, sizeof(iree_cpu_data_cache_));
  memcpy(iree_cpu_data_cache_, fields,
         iree_min(field_count, IREE_ARRAYSIZE(iree_cpu_data_cache_)) *
             sizeof(*iree_cpu_data_cache_));
}

const uint64_t* iree_cpu_data_fields(void) { return iree_cpu_data_cache_; }

uint64_t iree_cpu_data_field(iree_host_size_t field) {
  if (IREE_UNLIKELY(field >= IREE_ARRAYSIZE(iree_cpu_data_cache_))) return 0;
  return iree_cpu_data_cache_[field];
}

void iree_cpu_read_data(iree_host_size_t field_count, uint64_t* out_fields) {
  memset(out_fields, 0, field_count * sizeof(*out_fields));
  memcpy(out_fields, iree_cpu_data_cache_,
         iree_min(field_count, IREE_ARRAYSIZE(iree_cpu_data_cache_)) *
             sizeof(*out_fields));
}

//===----------------------------------------------------------------------===//
// Processor data lookup by key
//===----------------------------------------------------------------------===//

iree_status_t iree_cpu_lookup_data_by_key(iree_string_view_t key,
                                          int64_t* IREE_RESTRICT out_value) {
#define IREE_CPU_FEATURE_BIT(arch, field_index, bit_pos, bit_name, llvm_name) \
  if (IREE_ARCH_ENUM == IREE_ARCH_ENUM_##arch) {                              \
    if (iree_string_view_equal(key, IREE_SV(llvm_name))) {                    \
      *out_value = (iree_cpu_data_cache_[field_index] >> bit_pos) & 1;        \
      return iree_ok_status();                                                \
    }                                                                         \
  }
#include "iree/schemas/cpu_feature_bits.inl"
#undef IREE_CPU_FEATURE_BIT

  return iree_make_status(IREE_STATUS_NOT_FOUND,
                          "CPU feature '%.*s' unknown on %s", (int)key.size,
                          key.data, IREE_ARCH);
}

//===----------------------------------------------------------------------===//
// Processor identification
//===----------------------------------------------------------------------===//

#if defined(IREE_PLATFORM_ANDROID) || defined(IREE_PLATFORM_LINUX)

#include <sched.h>

iree_cpu_processor_id_t iree_cpu_query_processor_id(void) {
  // This path is relatively portable and should work on linux/bsd/etc-likes.
  // We may want to use getcpu when available so that we can get the group ID.
  // https://man7.org/linux/man-pages/man3/sched_getcpu.3.html
  //
  // libc implementations can use vDSO and other fun stuff to make this really
  // cheap: http://git.musl-libc.org/cgit/musl/tree/src/sched/sched_getcpu.c
  int id = sched_getcpu();
  return id != -1 ? id : 0;
}

#elif defined(IREE_PLATFORM_WINDOWS)

iree_cpu_processor_id_t iree_cpu_query_processor_id(void) {
  PROCESSOR_NUMBER pn;
  GetCurrentProcessorNumberEx(&pn);
  return 64 * pn.Group + pn.Number;
}

#else

// No implementation.
// We could allow an iree/base/config.h override to externalize this.
iree_cpu_processor_id_t iree_cpu_query_processor_id(void) { return 0; }

#endif  // IREE_PLATFORM_*

void iree_cpu_requery_processor_id(iree_cpu_processor_tag_t* IREE_RESTRICT tag,
                                   iree_cpu_processor_id_t* IREE_RESTRICT
                                       processor_id) {
  IREE_ASSERT_ARGUMENT(tag);
  IREE_ASSERT_ARGUMENT(processor_id);

  // TODO(benvanik): set a frequency for this and use a coarse timer
  // (CLOCK_MONOTONIC_COARSE) to do a ~4-10Hz refresh. We can store the last
  // query time and the last processor ID in the tag and only perform the query
  // if it has changed.

  *processor_id = iree_cpu_query_processor_id();
}
