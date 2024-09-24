#ifndef ZC_BASE_EXCEPTION_H
#define ZC_BASE_EXCEPTION_H


#include "src/zc/base/common.h"
#include "src/zc/containers/array.h"
#include "src/zc/strings/string.h"

namespace zc {

class ExceptionImpl;
template <typename T>
class Function;

class Exception {
  // Exception thrown in case of fatal errors.
  //
  // Actually, a subclass of this which also implements std::exception will be
  // thrown, but we hide that fact from the interface to avoid #including
  // <exception>.

 public:
  enum class Type {
    // What kind of failure?

    FAILED = 0,
    // Something went wrong. This is the usual error type. ZC_ASSERT and
    // ZC_REQUIRE throw this
    // error type.

    OVERLOADED = 1,
    // The call failed because of a temporary lack of resources. This could be
    // space resources
    // (out of memory, out of disk space) or time resources (request queue
    // overflow, operation
    // timed out).
    //
    // The operation might work if tried again, but it should NOT be repeated
    // immediately as this
    // may simply exacerbate the problem.

    DISCONNECTED = 2,
    // The call required communication over a connection that has been lost. The
    // callee will need
    // to re-establish connections and try again.

    UNIMPLEMENTED = 3
    // The requested method is not implemented. The caller may wish to revert to
    // a fallback
    // approach based on other methods.

    // IF YOU ADD A NEW VALUE:
    // - Update the stringifier.
    // - Update Cap'n Proto's RPC protocol's Exception.Type enum.
  };

  Exception(Type type, const char* file, int line,
            String description = nullptr) noexcept;
  Exception(Type type, String file, int line,
            String description = nullptr) noexcept;
  Exception(const Exception& other) noexcept;
  Exception(Exception&& other) = default;
  ~Exception() noexcept;

  const char* getFile() const { return file; }
  int getLine() const { return line; }
  Type getType() const { return type; }
  StringPtr getDescription() const { return description; }
  ArrayPtr<void* const> getStackTrace() const {
    return arrayPtr(trace, traceCount);
  }

  void setDescription(zc::String&& desc) { description = zc::mv(desc); }

  StringPtr getRemoteTrace() const { return remoteTrace; }
  void setRemoteTrace(zc::String&& value) { remoteTrace = zc::mv(value); }
  // Additional stack trace data originating from a remote server. If present,
  // then `getStackTrace()` only traces up until entry into the RPC system, and
  // the remote trace contains any trace information returned over the wire.
  // This string is human-readable but the format is otherwise unspecified.

  struct Context {
    // Describes a bit about what was going on when the exception was thrown.

    const char* file;
    int line;
    String description;
    Maybe<Own<Context>> next;

    Context(const char* file, int line, String&& description,
            Maybe<Own<Context>>&& next)
        : file(file),
          line(line),
          description(mv(description)),
          next(mv(next)) {}
    Context(const Context& other) noexcept;
  };

  inline Maybe<const Context&> getContext() const {
    ZC_IF_SOME(c, context) { return *c; }
    else {
      return zc::none;
    }
  }

  void wrapContext(const char* file, int line, String&& description);
  // Wraps the context in a new node.  This becomes the head node returned by
  // getContext() -- it is expected that contexts will be added in reverse order
  // as the exception passes up the callback stack.

  ZC_NOINLINE void extendTrace(uint32_t ignoreCount, uint limit = zc::maxValue);
  // Append the current stack trace to the exception's trace, ignoring the first
  // `ignoreCount` frames (see `getStackTrace()` for discussion of
  // `ignoreCount`).
  //
  // If `limit` is set, limit the number of frames added to the given number.

  ZC_NOINLINE void truncateCommonTrace();
  // Remove the part of the stack trace which the exception shares with the
  // caller of this method. This is used by the async library to remove the
  // async infrastructure from the stack trace before replacing it with the
  // async trace.

  void addTrace(void* ptr);
  // Append the given pointer to the backtrace, if it is not already full. This
  // is used by the async library to trace through the promise chain that led to
  // the exception.

  ZC_NOINLINE void addTraceHere();
  // Adds the location that called this method to the stack trace.

  using DetailTypeId = unsigned long long;
  struct Detail {
    DetailTypeId id;
    zc::Array<byte> value;
  };

  zc::Maybe<zc::ArrayPtr<const byte>> getDetail(DetailTypeId typeId) const;
  zc::ArrayPtr<const Detail> getDetails() const;
  void setDetail(DetailTypeId typeId, zc::Array<byte> value);
  zc::Maybe<zc::Array<byte>> releaseDetail(DetailTypeId typeId);
  // Details: Arbitrary extra information can be added to an exception.
  // Applications can define any kind of detail they want, but it must be
  // serializable to bytes so that it can be logged and transmitted over RPC.
  //
  // Every type of detail must have a unique ID, which is a 64-bit integer. It's
  // suggested that you use `capnp id` to generate these.
  //
  // It is expected that exceptions will rarely have more than one or two
  // details, so the implementation uses a flat array with O(n) lookup.
  //
  // The main use case for details is to be able to tunnel exceptions of a
  // different type through KJ / Cap'n Proto. In particular, Cloudflare Workers
  // commonly has to convert a JavaScript exception to KJ and back. The
  // exception is serialized using V8 serialization.

 private:
  String ownFile;
  const char* file;
  int line;
  Type type;
  String description;
  Maybe<Own<Context>> context;
  String remoteTrace;
  void* trace[32];
  uint32_t traceCount;

  bool isFullTrace = false;
  // Is `trace` a full trace to the top of the stack (or as close as we could
  // get before we ran out of space)? If this is false, then `trace` is instead
  // a partial trace covering just the frames between where the exception was
  // thrown and where it was caught.
  //
  // extendTrace() transitions this to true, and truncateCommonTrace() changes
  // it back to false.
  //
  // In theory, an exception should only hold a full trace when it is in the
  // process of being thrown via the C++ exception handling mechanism --
  // extendTrace() is called before the throw and truncateCommonTrace() after it
  // is caught. Note that when exceptions propagate through async promises, the
  // trace is extended one frame at a time instead, so isFullTrace should remain
  // false.

  zc::Vector<Detail> details;

  friend class ExceptionImpl;
};

struct CanceledException {};
// This exception is thrown to force-unwind a stack in order to immediately
// cancel whatever that stack was doing. It is used in the implementation of
// fibers in particular. Application code should almost never catch this
// exception, unless you need to modify stack unwinding for some reason.
// kj::runCatchingExceptions() does not catch it.

StringPtr KJ_STRINGIFY(Exception::Type type);
String KJ_STRINGIFY(const Exception& e);

// =======================================================================================

enum class LogSeverity {
  INFO,  // Information describing what the code is up to, which users may
         // request to see with a flag like `--verbose`.  Does not indicate a
         // problem.  Not printed by default; you must call setLogLevel(INFO) to
         // enable.
  WARNING,  // A problem was detected but execution can continue with correct
            // output.
  ERROR,  // Something is wrong, but execution can continue with garbage output.
  FATAL,  // Something went wrong, and execution cannot continue.
  DBG     // Temporary debug logging.  See ZC_DBG.

  // Make sure to update the stringifier if you add a new severity level.
};

StringPtr KJ_STRINGIFY(LogSeverity severity);

}  // namespace zc

#endif