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

#ifndef ZC_BASE_DEBUG_H
#define ZC_BASE_DEBUG_H

#include "src/zc/base/common.h"
#include "src/zc/base/exception.h"

namespace zc {

#if ZC_MSVC_TRADITIONAL_CPP
// MSVC does __VA_ARGS__ differently from GCC:
// - A trailing comma before an empty __VA_ARGS__ is removed automatically,
//   whereas GCC wants you to request this behavior with "##__VA_ARGS__".
// - If __VA_ARGS__ is passed directly as an argument to another macro, it will
//   be treated as a *single* argument rather than an argument list. This can be
//   worked around by wrapping the outer macro call in ZC_EXPAND(), which
//   apparently forces __VA_ARGS__ to be expanded before the macro is evaluated.
//   I don't understand the C preprocessor.
// - Using "#__VA_ARGS__" to stringify __VA_ARGS__ expands to zero tokens when
//   __VA_ARGS__ is empty, rather than expanding to an empty string literal. We
//   can work around by concatenating with an empty string literal.

#define ZC_EXPAND(X) X

#define KJ_LOG(severity, ...)                                          \
  for (bool _zc_shouldLog =                                            \
           ::zc::_::Debug::shouldLog(::zc::LogSeverity::severity);     \
       _zc_shouldLog; _zc_shouldLog = false)                           \
  ::zc::_::Debug::log(__FILE__, __LINE__, ::zc::LogSeverity::severity, \
                      "" #__VA_ARGS__, __VA_ARGS__)

#define KJ_DBG(...) KJ_EXPAND(KJ_LOG(DBG, __VA_ARGS__))

#define KJ_REQUIRE(cond, ...)                                          \
  if (auto _zcCondition = ::zc::_::MAGIC_ASSERT << cond) {             \
  } else                                                               \
    for (::zc::_::Debug::Fault f(                                      \
             __FILE__, __LINE__, ::zc::Exception::Type::FAILED, #cond, \
             "_zcCondition," #__VA_ARGS__, _zcCondition, __VA_ARGS__); \
         ; f.fatal())

#define KJ_FAIL_REQUIRE(...)                                           \
  for (::zc::_::Debug::Fault f(__FILE__, __LINE__,                     \
                               ::zc::Exception::Type::FAILED, nullptr, \
                               "" #__VA_ARGS__, __VA_ARGS__);          \
       ; f.fatal())

#define KJ_UNIMPLEMENTED(...)                                                 \
  for (::zc::_::Debug::Fault f(__FILE__, __LINE__,                            \
                               ::zc::Exception::Type::UNIMPLEMENTED, nullptr, \
                               "" #__VA_ARGS__, __VA_ARGS__);                 \
       ; f.fatal())
#else
#define ZC_LOG(severity, ...)                                          \
  for (bool _zc_shouldLog =                                            \
           ::zc::_::Debug::shouldLog(::zc::LogSeverity::severity);     \
       _zc_shouldLog; _zc_shouldLog = false)                           \
  ::zc::_::Debug::log(__FILE__, __LINE__, ::zc::LogSeverity::severity, \
                      #__VA_ARGS__, ##__VA_ARGS__)

#define ZC_DBG(...) ZC_LOG(DBG, ##__VA_ARGS__)

#define ZC_REQUIRE(cond, ...)                                            \
  if (auto _zcCondition = ::zc::_::MAGIC_ASSERT << cond) {               \
  } else                                                                 \
    for (::zc::_::Debug::Fault f(                                        \
             __FILE__, __LINE__, ::zc::Exception::Type::FAILED, #cond,   \
             "_zcCondition," #__VA_ARGS__, _zcCondition, ##__VA_ARGS__); \
         ; f.fatal())

#define ZC_FAIL_REQUIRE(...)                                           \
  for (::zc::_::Debug::Fault f(__FILE__, __LINE__,                     \
                               ::zc::Exception::Type::FAILED, nullptr, \
                               #__VA_ARGS__, ##__VA_ARGS__);           \
       ; f.fatal())

#define KJ_UNIMPLEMENTED(...)                                                 \
  for (::zc::_::Debug::Fault f(__FILE__, __LINE__,                            \
                               ::zc::Exception::Type::UNIMPLEMENTED, nullptr, \
                               #__VA_ARGS__, ##__VA_ARGS__);                  \
       ; f.fatal())
#endif

#define ZC_ASSERT ZC_REQUIRE
#define ZC_FAIL_ASSERT ZC_FAIL_REQUIRE
// Use "ASSERT" in place of "REQUIRE" when the problem is local to the immediate
// surrounding code. That is, if the assert ever fails, it indicates that the
// immediate surrounding code is broken.

namespace _ {  // private

class Debug {
 public:
  Debug() = delete;

  typedef LogSeverity Severity;  // backwards-compatibility

#if _WIN32 || __CYGWIN__
  struct Win32Result {
    uint number;
    inline explicit Win32Result(uint number) : number(number) {}
    operator bool() const { return number == 0; }
  };
#endif

  static inline bool shouldLog(LogSeverity severity) {
    return severity >= minSeverity;
  }
  // Returns whether messages of the given severity should be logged.

  static inline void setLogLevel(LogSeverity severity) {
    minSeverity = severity;
  }
  // Set the minimum message severity which will be logged.
  //
  // TODO(someday): Expose publicly.

  template <typename... Params>
  static void log(const char* file, int line, LogSeverity severity,
                  const char* macroArgs, Params&&... params);

  class Fault {
   public:
    template <typename Code, typename... Params>
    Fault(const char* file, int line, Code code, const char* condition,
          const char* macroArgs, Params&&... params);
    Fault(const char* file, int line, Exception::Type type,
          const char* condition, const char* macroArgs);
    Fault(const char* file, int line, int osErrorNumber, const char* condition,
          const char* macroArgs);
#if _WIN32 || __CYGWIN__
    Fault(const char* file, int line, Win32Result osErrorNumber,
          const char* condition, const char* macroArgs);
#endif
    ~Fault() noexcept(false);

    ZC_NOINLINE ZC_NORETURN void fatal();
    // Throw the exception.

   private:
    void init(const char* file, int line, Exception::Type type,
              const char* condition, const char* macroArgs,
              ArrayPtr<String> argValues);
    void init(const char* file, int line, int osErrorNumber,
              const char* condition, const char* macroArgs,
              ArrayPtr<String> argValues);
#if _WIN32 || __CYGWIN__
    void init(const char* file, int line, Win32Result osErrorNumber,
              const char* condition, const char* macroArgs,
              ArrayPtr<String> argValues);
#endif

    Exception* exception;
  };

  class SyscallResult {
   public:
    inline SyscallResult(int errorNumber) : errorNumber(errorNumber) {}
    inline operator void*() { return errorNumber == 0 ? this : nullptr; }
    inline int getErrorNumber() { return errorNumber; }

   private:
    int errorNumber;
  };

  template <typename Call>
  static SyscallResult syscall(Call&& call, bool nonblocking);
  template <typename Call>
  static int syscallError(Call&& call, bool nonblocking);

#if _WIN32 || __CYGWIN__
  static Win32Result win32Call(int boolean);
  static Win32Result win32Call(void* handle);
  static Win32Result winsockCall(int result);
  static uint getWin32ErrorCode();
#endif

  class Context : public ExceptionCallback {
   public:
    Context();
    ZC_DISALLOW_COPY_AND_MOVE(Context);
    virtual ~Context() noexcept(false);

    struct Value {
      const char* file;
      int line;
      String description;

      inline Value(const char* file, int line, String&& description)
          : file(file), line(line), description(mv(description)) {}
    };

    virtual Value evaluate() = 0;

    virtual void onRecoverableException(Exception&& exception) override;
    virtual void onFatalException(Exception&& exception) override;
    virtual void logMessage(LogSeverity severity, const char* file, int line,
                            int contextDepth, String&& text) override;

   private:
    bool logged;
    Maybe<Value> value;

    Value ensureInitialized();
  };

  template <typename Func>
  class ContextImpl : public Context {
   public:
    inline ContextImpl(Func& func) : func(func) {}
    ZC_DISALLOW_COPY_AND_MOVE(ContextImpl);

    Value evaluate() override { return func(); }

   private:
    Func& func;
  };

  template <typename... Params>
  static String makeDescription(const char* macroArgs, Params&&... params);

 private:
  static LogSeverity minSeverity;

  static void logInternal(const char* file, int line, LogSeverity severity,
                          const char* macroArgs, ArrayPtr<String> argValues);
  static String makeDescriptionInternal(const char* macroArgs,
                                        ArrayPtr<String> argValues);

  static int getOsErrorNumber(bool nonblocking);
  // Get the error code of the last error (e.g. from errno).  Returns -1 on
  // EINTR.
};

template <typename... Params>
void Debug::log(const char* file, int line, LogSeverity severity,
                const char* macroArgs, Params&&... params) {
  String argValues[sizeof...(Params)] = {str(params)...};
  logInternal(file, line, severity, macroArgs,
              arrayPtr(argValues, sizeof...(Params)));
}

template <>
inline void Debug::log<>(const char* file, int line, LogSeverity severity,
                         const char* macroArgs) {
  logInternal(file, line, severity, macroArgs, nullptr);
}

template <typename Code, typename... Params>
Debug::Fault::Fault(const char* file, int line, Code code,
                    const char* condition, const char* macroArgs,
                    Params&&... params)
    : exception(nullptr) {
  String argValues[sizeof...(Params)] = {str(params)...};
  init(file, line, code, condition, macroArgs,
       arrayPtr(argValues, sizeof...(Params)));
}

inline Debug::Fault::Fault(const char* file, int line, int osErrorNumber,
                           const char* condition, const char* macroArgs)
    : exception(nullptr) {
  init(file, line, osErrorNumber, condition, macroArgs, nullptr);
}

inline Debug::Fault::Fault(const char* file, int line, zc::Exception::Type type,
                           const char* condition, const char* macroArgs)
    : exception(nullptr) {
  init(file, line, type, condition, macroArgs, nullptr);
}

#if _WIN32 || __CYGWIN__
inline Debug::Fault::Fault(const char* file, int line,
                           Win32Result osErrorNumber, const char* condition,
                           const char* macroArgs)
    : exception(nullptr) {
  init(file, line, osErrorNumber, condition, macroArgs, nullptr);
}

inline Debug::Win32Result Debug::win32Call(int boolean) {
  return boolean ? Win32Result(0) : Win32Result(getWin32ErrorCode());
}
inline Debug::Win32Result Debug::win32Call(void* handle) {
  // Assume null and INVALID_HANDLE_VALUE mean failure.
  return win32Call(handle != nullptr && handle != (void*)-1);
}
inline Debug::Win32Result Debug::winsockCall(int result) {
  // Expect a return value of SOCKET_ERROR means failure.
  return win32Call(result != -1);
}
#endif

template <typename Call>
Debug::SyscallResult Debug::syscall(Call&& call, bool nonblocking) {
  while (call() < 0) {
    int errorNum = getOsErrorNumber(nonblocking);
    // getOsErrorNumber() returns -1 to indicate EINTR.
    // Also, if nonblocking is true, then it returns 0 on EAGAIN, which will
    // then be treated as a non-error.
    if (errorNum != -1) {
      return SyscallResult(errorNum);
    }
  }
  return SyscallResult(0);
}

template <typename Call>
int Debug::syscallError(Call&& call, bool nonblocking) {
  while (call() < 0) {
    int errorNum = getOsErrorNumber(nonblocking);
    // getOsErrorNumber() returns -1 to indicate EINTR.
    // Also, if nonblocking is true, then it returns 0 on EAGAIN, which will
    // then be treated as a non-error.
    if (errorNum != -1) {
      return errorNum;
    }
  }
  return 0;
}

template <typename... Params>
String Debug::makeDescription(const char* macroArgs, Params&&... params) {
  String argValues[sizeof...(Params)] = {str(params)...};
  return makeDescriptionInternal(macroArgs,
                                 arrayPtr(argValues, sizeof...(Params)));
}

template <>
inline String Debug::makeDescription<>(const char* macroArgs) {
  return makeDescriptionInternal(macroArgs, nullptr);
}

// =======================================================================================
// Magic Asserts!
//
// When ZC_ASSERT(foo == bar) fails, `foo` and `bar`'s actual values will be
// stringified in the error message. How does it work? We use template magic and
// operator precedence. The assertion actually evaluates something like this:
//
//     if (auto _zcCondition = zc::_::MAGIC_ASSERT << foo == bar)
//
// `<<` has operator precedence slightly above `==`, so `zc::_::MAGIC_ASSERT <<
// foo` gets evaluated first. This wraps `foo` in a little wrapper that captures
// the comparison operators and keeps enough information around to be able to
// stringify the left and right sides of the comparison independently. As
// always, the stringification only actually occurs if the assert fails.
//
// You might ask why we use operator `<<` and not e.g. operator `<=`, since
// operators of the same precedence are evaluated left-to-right. The answer is
// that some compilers trigger all sorts of warnings when you seem to be using a
// comparison as the input to another comparison. The particular warning GCC
// produces is its general "-Wparentheses" warning which is broadly useful, so
// we don't want to disable it. `<<` also produces some warnings, but only on
// Clang and the specific warning is one we're comfortable disabling (see
// below). This does mean that we have to explicitly overload `operator<<`
// ourselves to make sure using it in an assert still works.
//
// You might also ask, if we're using operator `<<` anyway, why not start it
// from the right, in which case it would bind after computing any `<<`
// operators that were actually in the user's code? I tried this, but it
// resulted in a somewhat broader warning from clang that I felt worse about
// disabling (a warning about `<<` precedence not applying specifically to
// overloads) and also created ambiguous overload errors in the ZC units code.

#if __clang__
// We intentionally overload operator << for the specific purpose of evaluating
// it before evaluating comparison expressions, so stop Clang from warning about
// it. Unfortunately this means eliminating a warning that would otherwise be
// useful for people using iostreams... sorry.
#pragma GCC diagnostic ignored "-Woverloaded-shift-op-parentheses"
#endif

template <typename T>
struct DebugExpression;

template <typename T, typename = decltype(toCharSequence(instance<T&>()))>
inline auto tryToCharSequence(T* value) {
  return zc::toCharSequence(*value);
}
inline StringPtr tryToCharSequence(...) { return "(can't stringify)"_zc; }
// SFINAE to stringify a value if and only if it can be stringified.

template <typename Left, typename Right>
struct DebugComparison {
  Left left;
  Right right;
  StringPtr op;
  bool result;

  inline operator bool() const { return ZC_LIKELY result; }

  template <typename T>
  inline void operator&(T&& other) = delete;
  template <typename T>
  inline void operator^(T&& other) = delete;
  template <typename T>
  inline void operator|(T&& other) = delete;
};

template <typename Left, typename Right>
String ZC_STRINGIFY(DebugComparison<Left, Right>& cmp) {
  return _::concat(tryToCharSequence(&cmp.left), cmp.op,
                   tryToCharSequence(&cmp.right));
}

template <typename T>
struct DebugExpression {
  DebugExpression(T&& value) : value(zc::fwd<T>(value)) {}
  T value;

  // Handle comparison operations by constructing a DebugComparison value.
#define DEFINE_OPERATOR(OP)                                                \
  template <typename U>                                                    \
  DebugComparison<T, U> operator OP(U && other) {                          \
    bool result = value OP other;                                          \
    return {zc::fwd<T>(value), zc::fwd<U>(other), " " #OP " "_zc, result}; \
  }
  DEFINE_OPERATOR(==);
  DEFINE_OPERATOR(!=);
  DEFINE_OPERATOR(<=);
  DEFINE_OPERATOR(>=);
  DEFINE_OPERATOR(<);
  DEFINE_OPERATOR(>);
#undef DEFINE_OPERATOR

  // Handle binary operators that have equal or lower precedence than
  // comparisons by performing the operation and wrapping the result.
#define DEFINE_OPERATOR(OP)                                                   \
  template <typename U>                                                       \
  inline auto operator OP(U&& other) {                                        \
    return DebugExpression<decltype(zc::fwd<T>(value) OP zc::fwd<U>(other))>( \
        zc::fwd<T>(value) OP zc::fwd<U>(other));                              \
  }
  DEFINE_OPERATOR(<<);
  DEFINE_OPERATOR(>>);
  DEFINE_OPERATOR(&);
  DEFINE_OPERATOR(^);
  DEFINE_OPERATOR(|);
#undef DEFINE_OPERATOR

  inline operator bool() {
    // No comparison performed, we're just asserting the expression is truthy.
    // This also covers the case of the logic operators && and || -- we cannot
    // overload those because doing so would break short-circuiting behavior.
    return value;
  }
};

template <typename T>
StringPtr ZC_STRINGIFY(const DebugExpression<T>& exp) {
  // Hack: This will only ever be called in cases where the expression's
  // truthiness was asserted directly, and was determined to be falsy.
  return "false"_zc;
}

struct DebugExpressionStart {
  template <typename T>
  DebugExpression<T> operator<<(T&& value) const {
    return DebugExpression<T>(zc::fwd<T>(value));
  }
};
static constexpr DebugExpressionStart MAGIC_ASSERT;

}  // namespace _
}  // namespace zc

#endif
