// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// Copyright (c) 2024 Zode.Z. All rights reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#ifndef ZC_BASE_COMMON_H
#define ZC_BASE_COMMON_H

#if defined(__GNUC__) || defined(__clang__)
#define ZC_BEGIN_SYSTEM_HEADER _Pragma("GCC system_header")
#elif defined(_MSC_VER)
#define ZC_BEGIN_SYSTEM_HEADER __pragma(warning(push, 0))
#define ZC_END_SYSTEM_HEADER __pragma(warning(pop))
#endif

#ifndef ZC_BEGIN_SYSTEM_HEADER
#define ZC_BEGIN_SYSTEM_HEADER
#endif

#ifndef ZC_END_SYSTEM_HEADER
#define ZC_END_SYSTEM_HEADER
#endif

#if !defined(ZC_HEADER_WARNINGS) || !ZC_HEADER_WARNINGS
#define ZC_BEGIN_HEADER ZC_BEGIN_SYSTEM_HEADER
#define ZC_END_HEADER ZC_END_SYSTEM_HEADER
#else
#define ZC_BEGIN_HEADER
#define ZC_END_HEADER
#endif

#ifdef __has_cpp_attribute
#define ZC_HAS_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
#define ZC_HAS_CPP_ATTRIBUTE(x) 0
#endif

#ifdef __has_feature
#define ZC_HAS_COMPILER_FEATURE(x) __has_feature(x)
#else
#define ZC_HAS_COMPILER_FEATURE(x) 0
#endif

#ifndef ZC_NO_COMPILER_CHECK
#if __cplusplus < 202002L && !__CDT_PARSER__
#error \
    "This code requires C++20. Either your compiler does not support it or it is not enabled."
#ifdef __GNUC__
// Compiler claims compatibility with GCC, so presumably supports -std.
#error "Pass -std=c++20 on the compiler command line to enable C++20."
#endif
#endif

#ifdef __GNUC__
#if __clang__
#if __clang_major__ < 14
#warning "This library requires at least Clang 14.0."
#endif
#if __cplusplus >= 202002L && !(__has_include(<coroutine>) || __has_include(<experimental/coroutine>))
#warning \
    "Your compiler supports C++20 but your C++ standard library does not.  If your "\
               "system has libc++ installed (as should be the case on e.g. Mac OSX), try adding "\
               "-stdlib=libc++ to your CXXFLAGS."
#endif
#else
#if __GNUC__ < 10
#warning "This library requires at least GCC 10.0."
#endif
#endif
#elif defined(_MSC_VER)
#if _MSC_VER < 1930 && !defined(__clang__)
#error "You need Visual Studio 2022 or better to compile this code."
#endif
#else
#warning \
    "I don't recognize your compiler. As of this writing, Clang, GCC, and Visual Studio "\
           "are the only known compilers with enough C++20 support for this library. "\
           "#define ZC_NO_COMPILER_CHECK to make this warning go away."
#endif
#endif

#if defined(__clang__)
#define ZC_CLANG
#elif defined(__GNUC__)
#define ZC_GCC
#elif defined(_MSC_VER)
#define ZC_MSVC
#endif

#include <cstddef>

namespace zc {

using uint = unsigned int;
using byte = unsigned char;
using uint32_t = uint;

// =======================================================================================
// Common macros, especially for common yet compiler-specific features.

// Detect whether RTTI and exceptions are enabled, assuming they are unless we
// have specific evidence to the contrary.  Clients can always define ZC_NO_RTTI
// explicitly to override the check. As of version 2, exceptions are required,
// so this produces an error otherwise.

// TODO: Ideally we'd use __cpp_exceptions/__cpp_rtti not being defined as the
//   first pass since that is the standard compliant way. However, it's unclear
//   how to use those macros (or any others) to distinguish between the compiler
//   supporting feature detection and the feature being disabled vs the compiler
//   not supporting feature detection at all.
#if defined(__has_feature)
#if !defined(ZC_NO_RTTI) && !__has_feature(cxx_rtti)
#define ZC_NO_RTTI 1
#endif
#if !__has_feature(cxx_exceptions)
#error "ZC requires C++ exceptions, please enable them"
#endif
#elif defined(__GNUC__)
#if !defined(ZC_NO_RTTI) && !__GXX_RTTI
#define ZC_NO_RTTI 1
#endif
#if !__EXCEPTIONS
#error "ZC requires C++ exceptions, please enable them"
#endif
#elif defined(_MSC_VER)
#if !defined(ZC_NO_RTTI) && !defined(_CPPRTTI)
#define ZC_NO_RTTI 1
#endif
#if !defined(_CPPUNWIND)
#error "ZC requires C++ exceptions, please enable them"
#endif
#endif

#if !defined(ZC_DEBUG) && !defined(ZC_NDEBUG)
// Heuristically decide whether to enable debug mode.  If DEBUG or NDEBUG is
// defined, use that. Otherwise, fall back to checking whether optimization is
// enabled.
#if defined(DEBUG) || defined(_DEBUG)
#define ZC_DEBUG
#elif defined(NDEBUG)
#define ZC_NDEBUG
#elif __OPTIMIZE__
#define ZC_NDEBUG
#else
#define ZC_DEBUG
#endif
#endif

#define ZC_DISALLOW_COPY(classname)     \
  classname(const classname&) = delete; \
  classname& operator=(const classname&) = delete
// Deletes the implicit copy constructor and assignment operator. This inhibits
// the compiler from generating the implicit move constructor and assignment
// operator for this class, but allows the code author to supply them, if they
// make sense to implement.
//
// This macro should not be your first choice. Instead, prefer using
// ZC_DISALLOW_COPY_AND_MOVE, and only use this macro when you have determined
// that you must implement move semantics for your type.

#define ZC_DISALLOW_COPY_AND_MOVE(classname)       \
  classname(const classname&) = delete;            \
  classname& operator=(const classname&) = delete; \
  classname(classname&&) = delete;                 \
  classname& operator=(classname&&) = delete
// Deletes the implicit copy and move constructors and assignment operators.
// This is useful in cases where the code author wants to provide an additional
// compile-time guard against subsequent maintainers casually adding move
// operations. This is particularly useful when implementing RAII classes that
// are intended to be completely immobile.

#define ZC_NOP ((void)0)

// C++20 Attributes
#define ZC_DEPRECATED(msg) [[deprecated(msg)]]
#define ZC_NORETURN [[noreturn]]
#define ZC_NODISCARD [[nodiscard]]
#define ZC_FALLTHROUGH [[fallthrough]]
#define ZC_MAYBE_UNUSED [[maybe_unused]]
#define ZC_LIKELY [[likely]]
#define ZC_UNLIKELY [[unlikely]]
#define ZC_NO_UNIQUE_ADDRESS [[no_unique_address]]

// Non-standard Attributes
#define ZC_ALIGNED(x) [[gnu::aligned(x)]]

#if defined(ZC_CLANG)
#define ZC_ALWAYS_INLINE \
  [[clang::always_inline]] inline  // Force a function to always be inlined.
                                   // Apply only to the prototype, not to the
                                   // definition.
#define ZC_NOINLINE [[clang::noinline]]
#define ZC_HOT [[gnu::hot]]
#define ZC_COLD [[gnu::cold]]
#define ZC_PACKED [[gnu::packed]]
#define ZC_UNROLL_LOOPS __attribute__((optimize("unroll-loops")))
#define ZC_ASSUME(cond) __builtin_assume(cond)
#elif defined(ZC_GCC)
#define ZC_ALWAYS_INLINE [[gnu::always_inline]] inline
#define ZC_NOINLINE [[gnu::noinline]]
#define ZC_HOT [[gnu::hot]]
#define ZC_COLD [[gnu::cold]]
#define ZC_PACKED [[gnu::packed]]
#define ZC_UNROLL_LOOPS __attribute__((optimize("unroll-loops")))
#define ZC_ASSUME(cond)          \
  do {                           \
    if (!cond) ZC_UNLIKELY {     \
        __builtin_unreachable(); \
      }                          \
  } while (false)
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

#if defined(_MSC_VER) && !__clang__
#define ZC_UNUSED
#define ZC_WARN_UNUSED_RESULT
// TODO(msvc): ZC_WARN_UNUSED_RESULT can use _Check_return_ on MSVC, but it's a prefix, so
//   wrapping the whole prototype is needed. http://msdn.microsoft.com/en-us/library/jj159529.aspx
//   Similarly, ZC_UNUSED could use __pragma(warning(suppress:...)), but again that's a prefix.
#else
#define ZC_UNUSED __attribute__((unused))
#define ZC_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#endif

#if ZC_HAS_CPP_ATTRIBUTE(clang::lifetimebound)
// If this is generating too many false-positives, the user is responsible for
// disabling the problematic warning at the compiler switch level or by
// suppressing the place where the false-positive is reported through
// compiler-specific pragmas if available.
#define ZC_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define ZC_LIFETIMEBOUND
#endif
// Annotation that indicates the returned value is referencing a resource owned
// by this type (e.g. cStr() on a std::string). Unfortunately this lifetime can
// only be superficial currently & cannot track further. For example, there's no
// way to get `array.asPtr().slice(5, 6))` to warn if the last slice exceeds the
// lifetime of `array`. That's because in the general case `ArrayPtr::slice`
// can't have the lifetime bound annotation since it's not wrong to do something
// like:
//     ArrayPtr<char> doSomething(ArrayPtr<char> foo) {
//        ...
//        return foo.slice(5, 6);
//     }
// If `ArrayPtr::slice` had a lifetime bound then the compiler would warn about
// this perfectly legitimate method. Really there needs to be 2 more
// annotations. One to inherit the lifetime bound and another to inherit the
// lifetime bound from a parameter (which really could be the same thing by
// allowing a syntax like `[[clang::lifetimebound(*this)]]`.
// https://clang.llvm.org/docs/AttributeReference.html#lifetimebound

#define ZC_CONCAT_(x, y) x##y
#define ZC_CONCAT(x, y) ZC_CONCAT_(x, y)
#define ZC_UNIQUE_NAME(prefix) ZC_CONCAT(prefix, __LINE__)
// Create a unique identifier name.  We use concatenate __LINE__ rather than
// __COUNTER__ so that the name can be used multiple times in the same macro.

namespace _ {

ZC_NORETURN void inlineRequireFailure(const char* file, int line,
                                      const char* expectation,
                                      const char* macroArgs,
                                      const char* message = nullptr);

ZC_NORETURN void unreachable();

}  // namespace _

#if _MSC_VER && !defined(__clang__) && \
    (!defined(_MSVC_TRADITIONAL) || _MSVC_TRADITIONAL)
#define ZC_MSVC_TRADITIONAL_CPP 1
#endif

#ifdef ZC_DEBUG
#if ZC_MSVC_TRADITIONAL_CPP
#define ZC_IREQUIRE(condition, ...)                               \
  if (ZC_LIKELY(condition))                                       \
    ;                                                             \
  else                                                            \
    ::zc::_::inlineRequireFailure(__FILE__, __LINE__, #condition, \
                                  "" #__VA_ARGS__, __VA_ARGS__)
// Version of ZC_DREQUIRE() which is safe to use in headers that are #included
// by users.  Used to check preconditions inside inline methods. ZC_IREQUIRE is
// particularly useful in that it will be enabled depending on whether the
// application is compiled in debug mode rather than whether libzc is.
#else
#define ZC_IREQUIRE(condition, ...)                               \
  if (condition)                                                  \
    ZC_LIKELY;                                                    \
  else                                                            \
    ::zc::_::inlineRequireFailure(__FILE__, __LINE__, #condition, \
                                  #__VA_ARGS__, ##__VA_ARGS__)
// Version of ZC_DREQUIRE() which is safe to use in headers that are #included
// by users.  Used to check preconditions inside inline methods. ZC_IREQUIRE is
// particularly useful in that it will be enabled depending on whether the
// application is compiled in debug mode rather than whether libzc is.
#endif
#else
#define ZC_IREQUIRE(condition, ...)
#endif

#define ZC_IASSERT ZC_IREQUIRE

#define ZC_UNREACHABLE ::zc::_::unreachable();
// Put this on code paths that cannot be reached to suppress compiler warnings
// about missing returns.

#if __clang__
#define ZC_KNOWN_UNREACHABLE(code)                                       \
  do {                                                                   \
    _Pragma("clang diagnostic push")                                     \
        _Pragma("clang diagnostic ignored \"-Wunreachable-code\"") code; \
    _Pragma("clang diagnostic pop")                                      \
  } while (false)
// Suppress "unreachable code" warnings on intentionally unreachable code.
#else
// TODO(someday): Add support for non-clang compilers.
#define ZC_KNOWN_UNREACHABLE(code) \
  do {                             \
    code;                          \
  } while (false)
#endif

// =======================================================================================
// Template meta programming helpers.

template <typename T>
struct NoInfer_ {
  using Type = T;
};
template <typename T>
using NoInfer = typename NoInfer_<T>::Type;
// Use NoInfer<T>::Type in place of T for a template function parameter to
// prevent inference of the type based on the parameter value.

template <typename T>
struct RemoveConst_ {
  typedef T Type;
};
template <typename T>
struct RemoveConst_<const T> {
  typedef T Type;
};
template <typename T>
using RemoveConst = typename RemoveConst_<T>::Type;

template <typename T>
struct Decay_ {
  typedef T Type;
};
template <typename T>
struct Decay_<T&> {
  typedef typename Decay_<T>::Type Type;
};
template <typename T>
struct Decay_<T&&> {
  typedef typename Decay_<T>::Type Type;
};
template <typename T>
struct Decay_<T[]> {
  typedef typename Decay_<T*>::Type Type;
};
template <typename T>
struct Decay_<const T[]> {
  typedef typename Decay_<const T*>::Type Type;
};
template <typename T, size_t s>
struct Decay_<T[s]> {
  typedef typename Decay_<T*>::Type Type;
};
template <typename T, size_t s>
struct Decay_<const T[s]> {
  typedef typename Decay_<const T*>::Type Type;
};
template <typename T>
struct Decay_<const T> {
  typedef typename Decay_<T>::Type Type;
};
template <typename T>
struct Decay_<volatile T> {
  typedef typename Decay_<T>::Type Type;
};
template <typename T>
using Decay = typename Decay_<T>::Type;

template <bool b>
struct EnableIf_;
template <>
struct EnableIf_<true> {
  typedef void Type;
};
template <bool b>
using EnableIf = typename EnableIf_<b>::Type;
// Use like:
//     template <typename T, typename = EnableIf<isValid<T>()>>
//     void func(T&& t);

template <typename T>
T instance() noexcept;
// Like std::declval, but doesn't transform T into an rvalue reference.  If you
// want that, specify instance<T&&>().

template <typename T>
struct IsConst_ {
  static constexpr bool value = false;
};
template <typename T>
struct IsConst_<const T> {
  static constexpr bool value = true;
};
template <typename T>
constexpr bool isConst() {
  return IsConst_<T>::value;
}

template <typename T>
struct EnableIfNotConst_ {
  typedef T Type;
};
template <typename T>
struct EnableIfNotConst_<const T>;
template <typename T>
using EnableIfNotConst = typename EnableIfNotConst_<T>::Type;

template <typename T>
struct RemoveConstOrDisable_ {
  struct Type;
};
template <typename T>
struct RemoveConstOrDisable_<const T> {
  typedef T Type;
};
template <typename T>
using RemoveConstOrDisable = typename RemoveConstOrDisable_<T>::Type;

template <typename T>
struct IsReference_ {
  static constexpr bool value = false;
};
template <typename T>
struct IsReference_<T&> {
  static constexpr bool value = true;
};
template <typename T>
constexpr bool isReference() {
  return IsReference_<T>::value;
}

template <typename From, typename To>
struct PropagateConst_ {
  using Type = To;
};
template <typename From, typename To>
struct PropagateConst_<const From, To> {
  using Type = const To;
};
template <typename From, typename To>
using PropagateConst = typename PropagateConst_<From, To>::Type;

// =======================================================================================
// Equivalents to std::move() and std::forward(), since these are very commonly
// needed and the std header <utility> pulls in lots of other stuff.
//
// We use abbreviated names mv and fwd because these helpers (especially mv) are
// so commonly used that the cost of typing more letters outweighs the cost of
// being slightly harder to understand when first encountered.

template <typename T>
constexpr T&& mv(T& t) noexcept {
  return static_cast<T&&>(t);
}
template <typename T>
constexpr T&& fwd(NoInfer<T>& t) noexcept {
  return static_cast<T&&>(t);
}

template <typename T>
constexpr T cp(T& t) noexcept {
  return t;
}
template <typename T>
constexpr T cp(const T& t) noexcept {
  return t;
}
// Useful to force a copy, particularly to pass into a function that expects
// T&&.

// =======================================================================================
// Maybe
//
// Use in cases where you want to indicate that a value may be null.  Using
// Maybe<T&> instead of T* forces the caller to handle the null case in order to
// satisfy the compiler, thus reliably preventing null pointer dereferences at
// runtime.
//
// Maybe<T> can be implicitly constructed from T and from zc::none.
// To read the value of a Maybe<T>, do:
//
//    ZC_IF_SOME(value, someFuncReturningMaybe()) {
//      doSomething(value);
//    } else {
//      maybeWasNone();
//    }
//
// ZC_IF_SOME's first parameter is a variable name which will be defined within
// the following block.  The variable will be a reference to the Maybe's value.
//
// Note that Maybe<T&> actually just wraps a pointer, whereas Maybe<T> wraps a T
// and a boolean indicating nullness.

template <typename T>
class Maybe;

namespace _ {  // private

template <typename T>
class NullableValue {
  // Class whose interface behaves much like T*, but actually contains an
  // instance of T and a boolean flag indicating nullness.

 public:
  inline NullableValue(NullableValue&& other) : isSet(other.isSet) {
    if (isSet) {
      ctor(value, zc::mv(other.value));
    }
  }
  inline NullableValue(const NullableValue& other) : isSet(other.isSet) {
    if (isSet) {
      ctor(value, other.value);
    }
  }
  inline NullableValue(NullableValue& other) : isSet(other.isSet) {
    if (isSet) {
      ctor(value, other.value);
    }
  }
  inline ~NullableValue()
#if _MSC_VER && !defined(__clang__)
      // TODO(msvc): MSVC has a hard time with noexcept specifier expressions
      // that are more complex
      //   than `true` or `false`. We had a workaround for VS2015, but VS2017
      //   regressed.
      noexcept(false)
#else
      noexcept(noexcept(instance<T&>().~T()))
#endif
  {
    if (isSet) {
      dtor(value);
    }
  }

  inline T& operator*() & { return value; }
  inline const T& operator*() const& { return value; }
  inline T&& operator*() && { return zc::mv(value); }
  inline const T&& operator*() const&& { return zc::mv(value); }
  inline T* operator->() { return &value; }
  inline const T* operator->() const { return &value; }
  inline operator T*() { return isSet ? &value : nullptr; }
  inline operator const T*() const { return isSet ? &value : nullptr; }

  template <typename... Params>
  inline T& emplace(Params&&... params) {
    if (isSet) {
      isSet = false;
      dtor(value);
    }
    ctor(value, zc::fwd<Params>(params)...);
    isSet = true;
    return value;
  }

  inline NullableValue() : isSet(false) {}
  inline NullableValue(T&& t) : isSet(true) { ctor(value, zc::mv(t)); }
  inline NullableValue(T& t) : isSet(true) { ctor(value, t); }
  inline NullableValue(const T& t) : isSet(true) { ctor(value, t); }
  template <typename U>
  inline NullableValue(NullableValue<U>&& other) : isSet(other.isSet) {
    if (isSet) {
      ctor(value, zc::mv(other.value));
    }
  }
  template <typename U>
  inline NullableValue(const NullableValue<U>& other) : isSet(other.isSet) {
    if (isSet) {
      ctor(value, other.value);
    }
  }
  template <typename U>
  inline NullableValue(const NullableValue<U&>& other) : isSet(other.isSet) {
    if (isSet) {
      ctor(value, *other.ptr);
    }
  }
  inline NullableValue(decltype(nullptr)) : isSet(false) {}

  inline NullableValue& operator=(NullableValue&& other) {
    if (&other != this) {
      // Careful about throwing destructors/constructors here.
      if (isSet) {
        isSet = false;
        dtor(value);
      }
      if (other.isSet) {
        ctor(value, zc::mv(other.value));
        isSet = true;
      }
    }
    return *this;
  }

  inline NullableValue& operator=(NullableValue& other) {
    if (&other != this) {
      // Careful about throwing destructors/constructors here.
      if (isSet) {
        isSet = false;
        dtor(value);
      }
      if (other.isSet) {
        ctor(value, other.value);
        isSet = true;
      }
    }
    return *this;
  }

  inline NullableValue& operator=(const NullableValue& other) {
    if (&other != this) {
      // Careful about throwing destructors/constructors here.
      if (isSet) {
        isSet = false;
        dtor(value);
      }
      if (other.isSet) {
        ctor(value, other.value);
        isSet = true;
      }
    }
    return *this;
  }

  inline NullableValue& operator=(T&& other) {
    emplace(zc::mv(other));
    return *this;
  }
  inline NullableValue& operator=(T& other) {
    emplace(other);
    return *this;
  }
  inline NullableValue& operator=(const T& other) {
    emplace(other);
    return *this;
  }
  template <typename U>
  inline NullableValue& operator=(NullableValue<U>&& other) {
    if (other.isSet) {
      emplace(zc::mv(other.value));
    } else {
      *this = nullptr;
    }
    return *this;
  }
  template <typename U>
  inline NullableValue& operator=(const NullableValue<U>& other) {
    if (other.isSet) {
      emplace(other.value);
    } else {
      *this = nullptr;
    }
    return *this;
  }
  template <typename U>
  inline NullableValue& operator=(const NullableValue<U&>& other) {
    if (other.isSet) {
      emplace(other.value);
    } else {
      *this = nullptr;
    }
    return *this;
  }
  inline NullableValue& operator=(decltype(nullptr)) {
    if (isSet) {
      isSet = false;
      dtor(value);
    }
    return *this;
  }

  inline bool operator==(decltype(nullptr)) const { return !isSet; }

  NullableValue(const T* t) = delete;
  NullableValue& operator=(const T* other) = delete;
  // We used to permit assigning a Maybe<T> directly from a T*, and the
  // assignment would check for nullness. This turned out never to be useful,
  // and sometimes to be dangerous.

 private:
  bool isSet;

#if _MSC_VER && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4624)
// Warns that the anonymous union has a deleted destructor when T is
// non-trivial. This warning seems broken.
#endif

  union {
    T value;
  };

#if _MSC_VER && !defined(__clang__)
#pragma warning(pop)
#endif

  friend class zc::Maybe<T>;
  template <typename U>
  friend NullableValue<U>&& readMaybe(Maybe<U>&& maybe);
};

template <typename T>
inline NullableValue<T>&& readMaybe(Maybe<T>&& maybe) {
  return zc::mv(maybe.ptr);
}
template <typename T>
inline T* readMaybe(Maybe<T>& maybe) {
  return maybe.ptr;
}
template <typename T>
inline const T* readMaybe(const Maybe<T>& maybe) {
  return maybe.ptr;
}
template <typename T>
inline T* readMaybe(Maybe<T&>&& maybe) {
  return maybe.ptr;
}
template <typename T>
inline T* readMaybe(const Maybe<T&>& maybe) {
  return maybe.ptr;
}

template <typename T>
inline T* readMaybe(T* ptr) {
  return ptr;
}
// Allow ZC_IF_SOME to work on regular pointers.

#if __GNUC__ || __clang__
// Both clang and GCC understand the GCC set of pragma directives.
#define ZC_SILENCE_DANGLING_ELSE_BEGIN \
  _Pragma("GCC diagnostic push")       \
      _Pragma("GCC diagnostic ignored \"-Wdangling-else\"")
#define ZC_SILENCE_DANGLING_ELSE_END _Pragma("GCC diagnostic pop")
#else  // __GNUC__
// I guess we'll find out if MSVC needs similar warning suppression.
#define ZC_SILENCE_DANGLING_ELSE_BEGIN
#define ZC_SILENCE_DANGLING_ELSE_END
#endif  // __GNUC__

}  // namespace _

#define ZC_IF_SOME(name, exp)                                 \
  ZC_SILENCE_DANGLING_ELSE_BEGIN                              \
  if (auto ZC_UNIQUE_NAME(_##name) = ::zc::_::readMaybe(exp)) \
    if (auto& name = *ZC_UNIQUE_NAME(_##name); false) {       \
    } else                                                    \
      ZC_SILENCE_DANGLING_ELSE_END

struct None {};
static constexpr None none;
// A "none" value solely for use in comparisons with and initializations of
// Maybes. `zc::none` will compare equal to all empty Maybes, and will compare
// not-equal to all non-empty Maybes. If you construct or assign to a Maybe from
// `zc::none`, the constructed/assigned Maybe will be empty.

#if __GNUC__ || __clang__
// These two macros provide a friendly syntax to extract the value of a Maybe or
// return early.
//
// Use ZC_UNWRAP_OR_RETURN if you just want to return a simple value when the
// Maybe is null:
//
//     int foo(Maybe<int> maybe) {
//       int value = ZC_UNWRAP_OR_RETURN(maybe, -1);
//       // ... use value ...
//     }
//
// For functions returning void, omit the second parameter to
// ZC_UNWRAP_OR_RETURN:
//
//     void foo(Maybe<int> maybe) {
//       int value = ZC_UNWRAP_OR_RETURN(maybe);
//       // ... use value ...
//     }
//
// Use ZC_UNWRAP_OR if you want to execute a block with multiple statements.
//
//     int foo(Maybe<int> maybe) {
//       int value = ZC_UNWRAP_OR(maybe, {
//         ZC_LOG(ERROR, "problem!!!");
//         return -1;
//       });
//       // ... use value ...
//     }
//
// The block MUST return at the end or you will get a compiler error
//
// Unfortunately, these macros seem impossible to express without using GCC's
// non-standard "statement expressions" extension. IIFEs don't do the trick here
// because a lambda cannot return out of the parent scope. These macros should
// therefore only be used in projects that target GCC or GCC-compatible
// compilers.
//
// `__GNUC__` is not defined when using LLVM's MSVC-compatible compiler driver
// `clang-cl` (even though clang supports the required extension), hence the
// additional `|| __clang__`.

#define ZC_UNWRAP_OR_RETURN(value, ...)          \
  (*({                                           \
    auto _zc_result = ::zc::_::readMaybe(value); \
    if (!_zc_result) {                           \
      return __VA_ARGS__;                        \
    }                                            \
    zc::mv(_zc_result);                          \
  }))

#define ZC_UNWRAP_OR(value, block)                             \
  (*({                                                         \
    auto _zc_result = ::zc::_::readMaybe(value);               \
    if (!_zc_result) {                                         \
      block;                                                   \
      asm("ZC_UNWRAP_OR_block_is_missing_return_statement\n"); \
    }                                                          \
    zc::mv(_zc_result);                                        \
  }))
#endif

template <typename T>
class Maybe {
  // A T, or nullptr.

  // IF YOU CHANGE THIS CLASS:  Note that there is a specialization of it in
  // memory.h.

 public:
  Maybe() : ptr(nullptr) {}
  Maybe(T&& t) : ptr(zc::mv(t)) {}
  Maybe(T& t) : ptr(t) {}
  Maybe(const T& t) : ptr(t) {}
  Maybe(Maybe&& other) : ptr(zc::mv(other.ptr)) { other = zc::none; }
  Maybe(const Maybe& other) : ptr(other.ptr) {}
  Maybe(Maybe& other) : ptr(other.ptr) {}

  template <typename U>
  Maybe(Maybe<U>&& other) {
    ZC_IF_SOME(val, zc::mv(other)) {
      ptr.emplace(zc::mv(val));
      other = zc::none;
    }
  }
  template <typename U>
  Maybe(Maybe<U&>&& other) {
    ZC_IF_SOME(val, other) {
      ptr.emplace(val);
      other = zc::none;
    }
  }
  template <typename U>
  Maybe(const Maybe<U>& other) {
    ZC_IF_SOME(val, other) { ptr.emplace(val); }
  }

  Maybe(zc::None) : ptr(nullptr) {}

  template <typename... Params>
  inline T& emplace(Params&&... params) {
    // Replace this Maybe's content with a new value constructed by passing the
    // given parameters to T's constructor. This can be used to initialize a
    // Maybe without copying or even moving a T. Returns a reference to the
    // newly-constructed value.

    return ptr.emplace(zc::fwd<Params>(params)...);
  }

  inline Maybe& operator=(T&& other) {
    ptr = zc::mv(other);
    return *this;
  }
  inline Maybe& operator=(T& other) {
    ptr = other;
    return *this;
  }
  inline Maybe& operator=(const T& other) {
    ptr = other;
    return *this;
  }

  inline Maybe& operator=(Maybe&& other) {
    ptr = zc::mv(other.ptr);
    other = zc::none;
    return *this;
  }
  inline Maybe& operator=(Maybe& other) {
    ptr = other.ptr;
    return *this;
  }
  inline Maybe& operator=(const Maybe& other) {
    ptr = other.ptr;
    return *this;
  }

  template <typename U>
  Maybe& operator=(Maybe<U>&& other) {
    ZC_IF_SOME(val, zc::mv(other)) {
      ptr.emplace(zc::mv(val));
      other = zc::none;
    }
    else {
      ptr = nullptr;
    }
    return *this;
  }
  template <typename U>
  Maybe& operator=(const Maybe<U>& other) {
    ZC_IF_SOME(val, other) { ptr.emplace(val); }
    else {
      ptr = nullptr;
    }
    return *this;
  }

  inline Maybe& operator=(zc::None) {
    ptr = nullptr;
    return *this;
  }
  inline bool operator==(zc::None) const { return ptr == nullptr; }

  inline bool operator==(const Maybe<T>& other) const {
    if (ptr == nullptr) {
      return other == zc::none;
    } else {
      return other.ptr != nullptr && *ptr == *other.ptr;
    }
  }

  Maybe(const T* t) = delete;
  Maybe& operator=(const T* other) = delete;
  // We used to permit assigning a Maybe<T> directly from a T*, and the
  // assignment would check for nullness. This turned out never to be useful,
  // and sometimes to be dangerous.

  T& orDefault(T& defaultValue) & {
    if (ptr == nullptr) {
      return defaultValue;
    } else {
      return *ptr;
    }
  }
  const T& orDefault(const T& defaultValue) const& {
    if (ptr == nullptr) {
      return defaultValue;
    } else {
      return *ptr;
    }
  }
  T&& orDefault(T&& defaultValue) && {
    if (ptr == nullptr) {
      return zc::mv(defaultValue);
    } else {
      return zc::mv(*ptr);
    }
  }
  const T&& orDefault(const T&& defaultValue) const&& {
    if (ptr == nullptr) {
      return zc::mv(defaultValue);
    } else {
      return zc::mv(*ptr);
    }
  }

  template <typename F,
            typename Result = decltype(instance<bool>() ? instance<T&>()
                                                        : instance<F>()())>
  Result orDefault(F&& lazyDefaultValue) & {
    if (ptr == nullptr) {
      return lazyDefaultValue();
    } else {
      return *ptr;
    }
  }

  template <typename F,
            typename Result = decltype(instance<bool>() ? instance<const T&>()
                                                        : instance<F>()())>
  Result orDefault(F&& lazyDefaultValue) const& {
    if (ptr == nullptr) {
      return lazyDefaultValue();
    } else {
      return *ptr;
    }
  }

  template <typename F,
            typename Result = decltype(instance<bool>() ? instance<T&&>()
                                                        : instance<F>()())>
  Result orDefault(F&& lazyDefaultValue) && {
    if (ptr == nullptr) {
      return lazyDefaultValue();
    } else {
      return zc::mv(*ptr);
    }
  }

  template <typename F,
            typename Result = decltype(instance<bool>() ? instance<const T&&>()
                                                        : instance<F>()())>
  Result orDefault(F&& lazyDefaultValue) const&& {
    if (ptr == nullptr) {
      return lazyDefaultValue();
    } else {
      return zc::mv(*ptr);
    }
  }

  template <typename Func>
  auto map(Func&& f) & -> Maybe<decltype(f(instance<T&>()))> {
    if (ptr == nullptr) {
      return zc::none;
    } else {
      return f(*ptr);
    }
  }

  template <typename Func>
  auto map(Func&& f) const& -> Maybe<decltype(f(instance<const T&>()))> {
    if (ptr == nullptr) {
      return zc::none;
    } else {
      return f(*ptr);
    }
  }

  template <typename Func>
  auto map(Func&& f) && -> Maybe<decltype(f(instance<T&&>()))> {
    if (ptr == nullptr) {
      return zc::none;
    } else {
      return f(zc::mv(*ptr));
    }
  }

  template <typename Func>
  auto map(Func&& f) const&& -> Maybe<decltype(f(instance<const T&&>()))> {
    if (ptr == nullptr) {
      return zc::none;
    } else {
      return f(zc::mv(*ptr));
    }
  }

 private:
  _::NullableValue<T> ptr;

  template <typename U>
  friend class Maybe;
  template <typename U>
  friend _::NullableValue<U>&& _::readMaybe(Maybe<U>&& maybe);
  template <typename U>
  friend U* _::readMaybe(Maybe<U>& maybe);
  template <typename U>
  friend const U* _::readMaybe(const Maybe<U>& maybe);
};

template <typename T>
class Maybe<T&> {
 public:
  constexpr Maybe() : ptr(nullptr) {}
  constexpr Maybe(T& t) : ptr(&t) {}
  constexpr Maybe(T* t) : ptr(t) {}

  inline constexpr Maybe(PropagateConst<T, Maybe>& other) : ptr(other.ptr) {}
  // Allow const copy only if `T` itself is const. Otherwise allow only
  // non-const copy, to protect transitive constness. Clang is happy for this
  // constructor to be declared `= default` since, after evaluation of
  // `PropagateConst`, it does end up being a default-able constructor. But, GCC
  // and MSVC both complain about that, claiming this constructor cannot be
  // declared default. I don't know who is correct, but whatever, we'll write
  // out an implementation, fine.
  //
  // Note that we can't solve this by inheriting DisallowConstCopyIfNotConst<T>
  // because we want to override the move constructor, and if we override the
  // move constructor then we must define the copy constructor here.

  inline constexpr Maybe(Maybe&& other) : ptr(other.ptr) {
    other.ptr = nullptr;
  }

  template <typename U>
  inline constexpr Maybe(Maybe<U&>& other) : ptr(other.ptr) {}
  template <typename U>
  inline constexpr Maybe(const Maybe<U&>& other)
      : ptr(const_cast<const U*>(other.ptr)) {}
  template <typename U>
  inline constexpr Maybe(Maybe<U&>&& other) : ptr(other.ptr) {
    other.ptr = nullptr;
  }
  template <typename U>
  inline constexpr Maybe(const Maybe<U&>&& other) = delete;
  template <typename U, typename = EnableIf<canConvert<U*, T*>()>>
  constexpr Maybe(Maybe<U>& other) : ptr(other.ptr.operator U*()) {}
  template <typename U, typename = EnableIf<canConvert<const U*, T*>()>>
  constexpr Maybe(const Maybe<U>& other) : ptr(other.ptr.operator const U*()) {}

  inline constexpr Maybe(zc::None) : ptr(nullptr) {}

  inline Maybe& operator=(T& other) {
    ptr = &other;
    return *this;
  }
  inline Maybe& operator=(T* other) {
    ptr = other;
    return *this;
  }
  inline Maybe& operator=(PropagateConst<T, Maybe>& other) {
    ptr = other.ptr;
    return *this;
  }
  inline Maybe& operator=(Maybe&& other) {
    ptr = other.ptr;
    other.ptr = nullptr;
    return *this;
  }
  template <typename U>
  inline Maybe& operator=(Maybe<U&>& other) {
    ptr = other.ptr;
    return *this;
  }
  template <typename U>
  inline Maybe& operator=(const Maybe<const U&>& other) {
    ptr = other.ptr;
    return *this;
  }
  template <typename U>
  inline Maybe& operator=(Maybe<U&>&& other) {
    ptr = other.ptr;
    other.ptr = nullptr;
    return *this;
  }
  template <typename U>
  inline Maybe& operator=(const Maybe<U&>&& other) = delete;

  inline bool operator==(zc::None) const { return ptr == nullptr; }

  T& orDefault(T& defaultValue) {
    if (ptr == nullptr) {
      return defaultValue;
    } else {
      return *ptr;
    }
  }
  const T& orDefault(const T& defaultValue) const {
    if (ptr == nullptr) {
      return defaultValue;
    } else {
      return *ptr;
    }
  }

  template <typename Func>
  auto map(Func&& f) -> Maybe<decltype(f(instance<T&>()))> {
    if (ptr == nullptr) {
      return zc::none;
    } else {
      return f(*ptr);
    }
  }

  template <typename Func>
  auto map(Func&& f) const -> Maybe<decltype(f(instance<const T&>()))> {
    if (ptr == nullptr) {
      return zc::none;
    } else {
      const T& ref = *ptr;
      return f(ref);
    }
  }

 private:
  T* ptr;

  template <typename U>
  friend class Maybe;
  template <typename U>
  friend U* _::readMaybe(Maybe<U&>&& maybe);
  template <typename U>
  friend U* _::readMaybe(const Maybe<U&>& maybe);
};

// =======================================================================================
// Casts

template <typename To, typename From>
To implicitCast(From&& from) {
  // `implicitCast<T>(value)` casts `value` to type `T` only if the conversion
  // is implicit.  Useful for e.g. resolving ambiguous overloads without
  // sacrificing type-safety.
  return zc::fwd<From>(from);
}

template <typename To, typename From>
To& downcast(From& from) {
  // Down-cast a value to a sub-type, asserting that the cast is valid.  In opt
  // mode this is a static_cast, but in debug mode (when RTTI is enabled) a
  // dynamic_cast will be used to verify that the value really has the requested
  // type.

  // Force a compile error if To is not a subtype of From.
  if (false) {
    zc::implicitCast<From*>(zc::implicitCast<To*>(nullptr));
  }

#if !ZC_NO_RTTI
  ZC_IREQUIRE(dynamic_cast<To*>(&from) != nullptr,
              "Value cannot be downcast() to requested type.");
#endif

  return static_cast<To&>(from);
}

}  // namespace zc

#endif  // ZC_BASE_COMMON_H