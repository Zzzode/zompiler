/*
 * Copyright 2024 Zode.Z. All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ZC_BASE_COMPILER_HINTS_H
#define ZC_BASE_COMPILER_HINTS_H

namespace zc {

static_assert(__cplusplus >= 202002L, "C++20 or later is required.");

#if defined(__clang__)
#define ZC_CLANG
#elif defined(__GNUC__)
#define ZC_GCC
#elif defined(_MSC_VER)
#define ZC_MSVC
#endif

#define ZC_NOP ((void)0)

// C++20 Attributes
#define ZC_DEPRECATED(msg) [[deprecated(msg)]]
#define ZC_NO_RETURN [[noreturn]]
#define ZC_NODISCARD [[nodiscard]]
#define ZC_FALLTHROUGH [[fallthrough]]
#define ZC_MAYBE_UNUSED [[maybe_unused]]
#define ZC_LIKELY [[likely]]
#define ZC_UNLIKELY [[unlikely]]
#define ZC_NO_UNIQUE_ADDRESS [[no_unique_address]]

// Non-standard Attributes
#define ZC_ALIGNED(x) [[gnu::aligned(x)]]

#if defined(ZC_CLANG)
#define ZC_ALWAYS_INLINE [[clang::always_inline]] inline
#define ZC_NOINLINE [[clang::noinline]]
#define ZC_HOT [[gnu::hot]]
#define ZC_COLD [[gnu::cold]]
#define ZC_PACKED [[gnu::packed]]
#define ZC_UNROLL_LOOPS __attribute__((optimize("unroll-loops")))
#define ZC_ASSUME(cond)                   \
  do {                                    \
    if (!(cond)) __builtin_unreachable(); \
  } while (0)
#elif defined(ZC_GCC)
#define ZC_ALWAYS_INLINE [[gnu::always_inline]] inline
#define ZC_NOINLINE [[gnu::noinline]]
#define ZC_HOT [[gnu::hot]]
#define ZC_COLD [[gnu::cold]]
#define ZC_PACKED [[gnu::packed]]
#define ZC_UNROLL_LOOPS __attribute__((optimize("unroll-loops")))
#define ZC_ASSUME(cond)                   \
  do {                                    \
    if (!(cond)) __builtin_unreachable(); \
  } while (0)
#elif defined(ZC_MSVC)
#define ZC_ALWAYS_INLINE __forceinline
#define ZC_NOINLINE __declspec(noinline)
#define ZC_HOT ZC_NOP
#define ZC_COLD ZC_NOP
#define ZC_PACKED ZC_NOP
#define ZC_UNROLL_LOOPS ZC_NOP
#define ZC_ASSUME(cond) __assume(cond)
#else
#define ZC_ALWAYS_INLINE ZC_NOP
#define ZC_NOINLINE ZC_NOP
#define ZC_HOT ZC_NOP
#define ZC_COLD ZC_NOP
#define ZC_PACKED ZC_NOP
#define ZC_UNROLL_LOOPS ZC_NOP
#define ZC_ASSUME(cond) ZC_NOP
#endif

#define ZC_NOEXCEPT noexcept
#define ZC_UNREACHABLE __builtin_unreachable()

#if !defined(ZC_NDEBUG) && defined(NDEBUG) && !defined(ZC_DEBUG)
#define ZC_NDEBUG 1
#endif

#if defined(__has_builtin) && !defined(__ibmxl__)
#if __has_builtin(__builtin_debugtrap)
#define ZC_DEBUG_TRAP() __builtin_debugtrap()
#elif __has_builtin(__debugbreak)
#define ZC_DEBUG_TRAP() __debugbreak()
#endif
#endif

#if !defined(ZC_DEBUG_TRAP)
#if defined(_MSC_VER) || defined(__INTEL_COMPILER)
#define ZC_DEBUG_TRAP() __debugbreak()
#elif defined(__ARMCC_VERSION)
#define ZC_DEBUG_TRAP() __breakpoint(42)
#elif defined(__ibmxl__) || defined(__xlC__)
#include <builtins.h>
#define ZC_DEBUG_TRAP() __trap(42)
#elif defined(__DMC__) && defined(_M_IX86)
#define ZC_DEBUG_TRAP() \
  do {                  \
    __asm int 3h;       \
  } while (0)
#elif defined(__i386__) || defined(__x86_64__)
#define ZC_DEBUG_TRAP()                \
  do {                                 \
    __asm__ __volatile__("int $0x03"); \
  } while (0)
#elif defined(__thumb__)
#define ZC_DEBUG_TRAP()                   \
  do {                                    \
    __asm__ __volatile__(".inst 0xde01"); \
  } while (0)
#elif defined(__aarch64__)
#define ZC_DEBUG_TRAP()                       \
  do {                                        \
    __asm__ __volatile__(".inst 0xd4200000"); \
  } while (0)
#elif defined(__arm__)
#define ZC_DEBUG_TRAP()                       \
  do {                                        \
    __asm__ __volatile__(".inst 0xe7f001f0"); \
  } while (0)
#elif defined(__alpha__) && !defined(__osf__)
#define ZC_DEBUG_TRAP()          \
  do {                           \
    __asm__ __volatile__("bpt"); \
  } while (0)
#elif defined(_54_)
#define ZC_DEBUG_TRAP()            \
  do {                             \
    __asm__ __volatile__("ESTOP"); \
  } while (0)
#elif defined(_55_)
#define ZC_DEBUG_TRAP()                                                      \
  do {                                                                       \
    __asm__ __volatile__(                                                    \
        ";\n .if (.MNEMONIC)\n ESTOP_1\n .else\n ESTOP_1()\n .endif\n NOP"); \
  } while (0)
#elif defined(_64P_)
#define ZC_DEBUG_TRAP()             \
  do {                              \
    __asm__ __volatile__("SWBP 0"); \
  } while (0)
#elif defined(_6x_)
#define ZC_DEBUG_TRAP()                             \
  do {                                              \
    __asm__ __volatile__("NOP\n .word 0x10000000"); \
  } while (0)
#elif defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0) && defined(__GNUC__)
#define ZC_DEBUG_TRAP() __builtin_trap()
#else
#include <signal.h>
#if defined(SIGTRAP)
#define ZC_DEBUG_TRAP() raise(SIGTRAP)
#else
#define ZC_DEBUG_TRAP() raise(SIGABRT)
#endif
#endif
#endif

#if !defined(ZC_NDEBUG) || (ZC_NDEBUG == 0)
#define ZC_DEBUG_ASSERT(expr) \
  do {                        \
    if (!expr) ZC_UNLIKELY {  \
        ZC_DEBUG_TRAP();      \
      }                       \
  } while (0)
#else
#define ZC_DEBUG_ASSERT(expr)
#endif

}  // namespace zc

#endif  // ZC_BASE_COMPILER_HINTS_H