// Copyright (c) 2013-2017 Sandstorm Development Group, Inc. and contributors
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

#if _WIN32
// Request Vista-level APIs.
#include <src/zc/core/win32-api-version.h>
#endif

#include <src/zc/core/filesystem.h>

#include <deque>

#include "src/zc/async/async-io-internal.h"
#include "src/zc/async/async-io.h"
#include "src/zc/core/debug.h"
#include "src/zc/core/io.h"
#include "src/zc/core/one-of.h"
#include "src/zc/core/vector.h"

#if _WIN32
#include <src/zc/core/windows-sanity.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#define inet_pton InetPtonA
#define inet_ntop InetNtopA
#include <io.h>
#define dup _dup
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace zc {

Promise<void> AsyncInputStream::read(void* buffer, size_t bytes) {
  return read(buffer, bytes, bytes).then([](size_t) {});
}

Promise<size_t> AsyncInputStream::read(void* buffer, size_t minBytes, size_t maxBytes) {
  return tryRead(buffer, minBytes, maxBytes).then([=](size_t result) {
    if (result >= minBytes) {
      return result;
    } else {
      zc::throwRecoverableException(ZC_EXCEPTION(DISCONNECTED, "stream disconnected prematurely"));
      // Pretend we read zeros from the input.
      memset(reinterpret_cast<byte*>(buffer) + result, 0, minBytes - result);
      return minBytes;
    }
  });
}

Maybe<uint64_t> AsyncInputStream::tryGetLength() { return zc::none; }

void AsyncInputStream::registerAncillaryMessageHandler(
    Function<void(ArrayPtr<AncillaryMessage>)> fn) {
  ZC_UNIMPLEMENTED("registerAncillaryMsgHandler is not implemented by this AsyncInputStream");
}

Maybe<Own<AsyncInputStream>> AsyncInputStream::tryTee(uint64_t) { return zc::none; }

zc::Promise<size_t> NullStream::tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
  return zc::constPromise<size_t, 0>();
}
zc::Maybe<uint64_t> NullStream::tryGetLength() { return uint64_t(0); }
zc::Promise<uint64_t> NullStream::pumpTo(zc::AsyncOutputStream& output, uint64_t amount) {
  return zc::constPromise<uint64_t, 0>();
}

zc::Promise<void> NullStream::write(ArrayPtr<const byte> buffer) { return zc::READY_NOW; }
zc::Promise<void> NullStream::write(zc::ArrayPtr<const zc::ArrayPtr<const byte>> pieces) {
  return zc::READY_NOW;
}
zc::Promise<void> NullStream::whenWriteDisconnected() { return zc::NEVER_DONE; }

void NullStream::shutdownWrite() {}

namespace {

class AsyncPump {
public:
  AsyncPump(AsyncInputStream& input, AsyncOutputStream& output, uint64_t limit, uint64_t doneSoFar)
      : input(input), output(output), limit(limit), doneSoFar(doneSoFar) {}

  Promise<uint64_t> pump() {
    // TODO(perf): This could be more efficient by reading half a buffer at a time and then
    //   starting the next read concurrent with writing the data from the previous read.

    uint64_t n = zc::min(limit - doneSoFar, sizeof(buffer));
    if (n == 0) return doneSoFar;

    return input.tryRead(buffer, 1, n).then([this](size_t amount) -> Promise<uint64_t> {
      if (amount == 0) return doneSoFar;  // EOF
      doneSoFar += amount;
      return output.write(arrayPtr(buffer).first(amount)).then([this]() { return pump(); });
    });
  }

private:
  AsyncInputStream& input;
  AsyncOutputStream& output;
  uint64_t limit;
  uint64_t doneSoFar;
  byte buffer[4096];
};

}  // namespace

Promise<uint64_t> unoptimizedPumpTo(AsyncInputStream& input, AsyncOutputStream& output,
                                    uint64_t amount, uint64_t completedSoFar) {
  auto pump = heap<AsyncPump>(input, output, amount, completedSoFar);
  auto promise = pump->pump();
  return promise.attach(zc::mv(pump));
}

Promise<uint64_t> AsyncInputStream::pumpTo(AsyncOutputStream& output, uint64_t amount) {
  // See if output wants to dispatch on us.
  ZC_IF_SOME(result, output.tryPumpFrom(*this, amount)) { return zc::mv(result); }

  // OK, fall back to naive approach.
  return unoptimizedPumpTo(*this, output, amount);
}

namespace {

class AllReader {
public:
  AllReader(AsyncInputStream& input) : input(input) {}

  Promise<Array<byte>> readAllBytes(uint64_t limit) {
    return loop(limit).then([this, limit](uint64_t headroom) {
      auto out = heapArray<byte>(limit - headroom);
      copyInto(out);
      return out;
    });
  }

  Promise<String> readAllText(uint64_t limit) {
    return loop(limit).then([this, limit](uint64_t headroom) {
      auto out = heapArray<char>(limit - headroom + 1);
      copyInto(out.first(out.size() - 1).asBytes());
      out.back() = '\0';
      return String(zc::mv(out));
    });
  }

private:
  AsyncInputStream& input;
  Vector<Array<byte>> parts;

  Promise<uint64_t> loop(uint64_t limit) {
    ZC_REQUIRE(limit > 0, "Reached limit before EOF.");

    auto part = heapArray<byte>(zc::min(4096, limit));
    auto partPtr = part.asPtr();
    parts.add(zc::mv(part));
    return input.tryRead(partPtr.begin(), partPtr.size(), partPtr.size())
        .then([this, ZC_CPCAP(partPtr), limit](size_t amount) mutable -> Promise<uint64_t> {
          limit -= amount;
          if (amount < partPtr.size()) {
            return limit;
          } else {
            return loop(limit);
          }
        });
  }

  void copyInto(ArrayPtr<byte> out) {
    size_t pos = 0;
    for (auto& part : parts) {
      size_t n = zc::min(part.size(), out.size() - pos);
      memcpy(out.begin() + pos, part.begin(), n);
      pos += n;
    }
  }
};

}  // namespace

Promise<Array<byte>> AsyncInputStream::readAllBytes(uint64_t limit) {
  auto reader = zc::heap<AllReader>(*this);
  auto promise = reader->readAllBytes(limit);
  return promise.attach(zc::mv(reader));
}

Promise<String> AsyncInputStream::readAllText(uint64_t limit) {
  auto reader = zc::heap<AllReader>(*this);
  auto promise = reader->readAllText(limit);
  return promise.attach(zc::mv(reader));
}

Maybe<Promise<uint64_t>> AsyncOutputStream::tryPumpFrom(AsyncInputStream& input, uint64_t amount) {
  return zc::none;
}

namespace {

class AsyncPipe final : public AsyncCapabilityStream, public Refcounted {
public:
  ~AsyncPipe() noexcept(false) {
    ZC_REQUIRE(
        state == zc::none || ownState.get() != nullptr,
        "destroying AsyncPipe with operation still in-progress; probably going to segfault") {
      // Don't std::terminate().
      break;
    }
  }

  Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    if (minBytes == 0) {
      return constPromise<size_t, 0>();
    } else
      ZC_IF_SOME(s, state) { return s.tryRead(buffer, minBytes, maxBytes); }
    else {
      return newAdaptedPromise<ReadResult, BlockedRead>(
                 *this, arrayPtr(reinterpret_cast<byte*>(buffer), maxBytes), minBytes)
          .then([](ReadResult r) { return r.byteCount; });
    }
  }

  Promise<ReadResult> tryReadWithFds(void* buffer, size_t minBytes, size_t maxBytes,
                                     AutoCloseFd* fdBuffer, size_t maxFds) override {
    if (minBytes == 0) {
      return ReadResult{0, 0};
    } else
      ZC_IF_SOME(s, state) {
        return s.tryReadWithFds(buffer, minBytes, maxBytes, fdBuffer, maxFds);
      }
    else {
      return newAdaptedPromise<ReadResult, BlockedRead>(
          *this, arrayPtr(reinterpret_cast<byte*>(buffer), maxBytes), minBytes,
          zc::arrayPtr(fdBuffer, maxFds));
    }
  }

  Promise<ReadResult> tryReadWithStreams(void* buffer, size_t minBytes, size_t maxBytes,
                                         Own<AsyncCapabilityStream>* streamBuffer,
                                         size_t maxStreams) override {
    if (minBytes == 0) {
      return ReadResult{0, 0};
    } else
      ZC_IF_SOME(s, state) {
        return s.tryReadWithStreams(buffer, minBytes, maxBytes, streamBuffer, maxStreams);
      }
    else {
      return newAdaptedPromise<ReadResult, BlockedRead>(
          *this, arrayPtr(reinterpret_cast<byte*>(buffer), maxBytes), minBytes,
          zc::arrayPtr(streamBuffer, maxStreams));
    }
  }

  Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
    if (amount == 0) {
      return constPromise<uint64_t, 0>();
    } else
      ZC_IF_SOME(s, state) { return s.pumpTo(output, amount); }
    else { return newAdaptedPromise<uint64_t, BlockedPumpTo>(*this, output, amount); }
  }

  void abortRead() override {
    ZC_IF_SOME(s, state) { s.abortRead(); }
    else {
      ownState = zc::heap<AbortedRead>();
      state = *ownState;

      readAborted = true;
      ZC_IF_SOME(f, readAbortFulfiller) {
        f->fulfill();
        readAbortFulfiller = zc::none;
      }
    }
  }

  Promise<void> write(ArrayPtr<const byte> buffer) override {
    if (buffer == nullptr) {
      return READY_NOW;
    } else
      ZC_IF_SOME(s, state) { return s.write(buffer); }
    else { return newAdaptedPromise<void, BlockedWrite>(*this, buffer, nullptr); }
  }

  Promise<void> write(ArrayPtr<const ArrayPtr<const byte>> pieces) override {
    while (pieces.size() > 0 && pieces[0].size() == 0) { pieces = pieces.slice(1, pieces.size()); }

    if (pieces.size() == 0) {
      return zc::READY_NOW;
    } else
      ZC_IF_SOME(s, state) { return s.write(pieces); }
    else {
      return newAdaptedPromise<void, BlockedWrite>(*this, pieces[0],
                                                   pieces.slice(1, pieces.size()));
    }
  }

  Promise<void> writeWithFds(ArrayPtr<const byte> data,
                             ArrayPtr<const ArrayPtr<const byte>> moreData,
                             ArrayPtr<const int> fds) override {
    while (data.size() == 0 && moreData.size() > 0) {
      data = moreData.front();
      moreData = moreData.slice(1, moreData.size());
    }

    if (data.size() == 0) {
      ZC_REQUIRE(fds.size() == 0, "can't attach FDs to empty message");
      return READY_NOW;
    } else
      ZC_IF_SOME(s, state) { return s.writeWithFds(data, moreData, fds); }
    else { return newAdaptedPromise<void, BlockedWrite>(*this, data, moreData, fds); }
  }

  Promise<void> writeWithStreams(ArrayPtr<const byte> data,
                                 ArrayPtr<const ArrayPtr<const byte>> moreData,
                                 Array<Own<AsyncCapabilityStream>> streams) override {
    while (data.size() == 0 && moreData.size() > 0) {
      data = moreData.front();
      moreData = moreData.slice(1, moreData.size());
    }

    if (data.size() == 0) {
      ZC_REQUIRE(streams.size() == 0, "can't attach capabilities to empty message");
      return READY_NOW;
    } else
      ZC_IF_SOME(s, state) { return s.writeWithStreams(data, moreData, zc::mv(streams)); }
    else { return newAdaptedPromise<void, BlockedWrite>(*this, data, moreData, zc::mv(streams)); }
  }

  Maybe<Promise<uint64_t>> tryPumpFrom(AsyncInputStream& input, uint64_t amount) override {
    if (amount == 0) {
      return constPromise<uint64_t, 0>();
    } else
      ZC_IF_SOME(s, state) { return s.tryPumpFrom(input, amount); }
    else { return newAdaptedPromise<uint64_t, BlockedPumpFrom>(*this, input, amount); }
  }

  Promise<void> whenWriteDisconnected() override {
    if (readAborted) {
      return zc::READY_NOW;
    } else
      ZC_IF_SOME(p, readAbortPromise) { return p.addBranch(); }
    else {
      auto paf = newPromiseAndFulfiller<void>();
      readAbortFulfiller = zc::mv(paf.fulfiller);
      auto fork = paf.promise.fork();
      auto result = fork.addBranch();
      readAbortPromise = zc::mv(fork);
      return result;
    }
  }

  void shutdownWrite() override {
    ZC_IF_SOME(s, state) { s.shutdownWrite(); }
    else {
      ownState = zc::heap<ShutdownedWrite>();
      state = *ownState;
    }
  }

private:
  Maybe<AsyncCapabilityStream&> state;
  // Object-oriented state! If any method call is blocked waiting on activity from the other end,
  // then `state` is non-null and method calls should be forwarded to it. If no calls are
  // outstanding, `state` is null.

  zc::Own<AsyncCapabilityStream> ownState;

  bool readAborted = false;
  Maybe<Own<PromiseFulfiller<void>>> readAbortFulfiller = zc::none;
  Maybe<ForkedPromise<void>> readAbortPromise = zc::none;

  void endState(AsyncIoStream& obj) {
    ZC_IF_SOME(s, state) {
      if (&s == &obj) { state = zc::none; }
    }
  }

  template <typename F>
  static auto teeExceptionVoid(F& fulfiller, Canceler& canceler) {
    // Returns a functor that can be passed as the second parameter to .then() to propagate the
    // exception to a given fulfiller. The functor's return type is void.
    //
    // All use cases of this helper below are also wrapped in `canceler.wrap()`, and fulfilling
    // `fulfiller` may cause the canceler to be canceled. It's possible the canceler will be
    // canceled before the exception even gets a chance to propagate out of the wrapped promise,
    // which would have the effet of replacing the original exception with a useless
    // "operation canceled" exception. To avoid this, we must release the canceler before
    // fulfilling the fulfiller.
    return [&fulfiller, &canceler](zc::Exception&& e) {
      canceler.release();
      fulfiller.reject(zc::cp(e));
      zc::throwRecoverableException(zc::mv(e));
    };
  }
  template <typename F>
  static auto teeExceptionSize(F& fulfiller, Canceler& canceler) {
    // Returns a functor that can be passed as the second parameter to .then() to propagate the
    // exception to a given fulfiller. The functor's return type is size_t.
    //
    // All use cases of this helper below are also wrapped in `canceler.wrap()`, and fulfilling
    // `fulfiller` may cause the canceler to be canceled. It's possible the canceler will be
    // canceled before the exception even gets a chance to propagate out of the wrapped promise,
    // which would have the effet of replacing the original exception with a useless
    // "operation canceled" exception. To avoid this, we must release the canceler before
    // fulfilling the fulfiller.
    return [&fulfiller, &canceler](zc::Exception&& e) -> size_t {
      canceler.release();
      fulfiller.reject(zc::cp(e));
      zc::throwRecoverableException(zc::mv(e));
      return 0;
    };
  }
  template <typename T, typename F>
  static auto teeExceptionPromise(F& fulfiller, Canceler& canceler) {
    // Returns a functor that can be passed as the second parameter to .then() to propagate the
    // exception to a given fulfiller. The functor's return type is Promise<T>.
    //
    // All use cases of this helper below are also wrapped in `canceler.wrap()`, and fulfilling
    // `fulfiller` may cause the canceler to be canceled. It's possible the canceler will be
    // canceled before the exception even gets a chance to propagate out of the wrapped promise,
    // which would have the effet of replacing the original exception with a useless
    // "operation canceled" exception. To avoid this, we must release the canceler before
    // fulfilling the fulfiller.
    return [&fulfiller, &canceler](zc::Exception&& e) -> zc::Promise<T> {
      canceler.release();
      fulfiller.reject(zc::cp(e));
      return zc::mv(e);
    };
  }

  class BlockedWrite final : public AsyncCapabilityStream {
    // AsyncPipe state when a write() is currently waiting for a corresponding read().

  public:
    BlockedWrite(PromiseFulfiller<void>& fulfiller, AsyncPipe& pipe,
                 ArrayPtr<const byte> writeBuffer, ArrayPtr<const ArrayPtr<const byte>> morePieces,
                 zc::OneOf<ArrayPtr<const int>, Array<Own<AsyncCapabilityStream>>> capBuffer = {})
        : fulfiller(fulfiller),
          pipe(pipe),
          writeBuffer(writeBuffer),
          morePieces(morePieces),
          capBuffer(zc::mv(capBuffer)) {
      ZC_REQUIRE(pipe.state == zc::none);
      pipe.state = *this;
    }

    ~BlockedWrite() noexcept(false) { pipe.endState(*this); }

    Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
      ZC_SWITCH_ONEOF(tryReadImpl(buffer, minBytes, maxBytes)) {
        ZC_CASE_ONEOF(done, Done) { return done.result; }
        ZC_CASE_ONEOF(retry, Retry) {
          return pipe.tryRead(retry.buffer, retry.minBytes, retry.maxBytes)
              .then([n = retry.alreadyRead](size_t amount) { return amount + n; });
        }
      }
      ZC_UNREACHABLE;
    }

    Promise<ReadResult> tryReadWithFds(void* buffer, size_t minBytes, size_t maxBytes,
                                       AutoCloseFd* fdBuffer, size_t maxFds) override {
      size_t capCount = 0;
      {  // TODO(cleanup): Remove redundant braces when we update to C++17.
        ZC_SWITCH_ONEOF(capBuffer) {
          ZC_CASE_ONEOF(fds, ArrayPtr<const int>) {
            capCount = zc::max(fds.size(), maxFds);
            // Unfortunately, we have to dup() each FD, because the writer doesn't release ownership
            // by default.
            // TODO(perf): Should we add an ownership-releasing version of writeWithFds()?
            for (auto i : zc::zeroTo(capCount)) {
              int duped;
              ZC_SYSCALL(duped = dup(fds[i]));
              fdBuffer[i] = zc::AutoCloseFd(fds[i]);
            }
            fdBuffer += capCount;
            maxFds -= capCount;
          }
          ZC_CASE_ONEOF(streams, Array<Own<AsyncCapabilityStream>>) {
            if (streams.size() > 0 && maxFds > 0) {
              // TODO(someday): We could let people pass a LowLevelAsyncIoProvider to
              //   newTwoWayPipe() if we wanted to auto-wrap FDs, but does anyone care?
              ZC_FAIL_REQUIRE(
                  "async pipe message was written with streams attached, but corresponding read "
                  "asked for FDs, and we don't know how to convert here");
            }
          }
        }
      }

      // Drop any unclaimed caps. This mirrors the behavior of unix sockets, where if we didn't
      // provide enough buffer space for all the written FDs, the remaining ones are lost.
      capBuffer = {};

      ZC_SWITCH_ONEOF(tryReadImpl(buffer, minBytes, maxBytes)) {
        ZC_CASE_ONEOF(done, Done) { return ReadResult{done.result, capCount}; }
        ZC_CASE_ONEOF(retry, Retry) {
          return pipe.tryReadWithFds(retry.buffer, retry.minBytes, retry.maxBytes, fdBuffer, maxFds)
              .then([byteCount = retry.alreadyRead, capCount](ReadResult result) {
                result.byteCount += byteCount;
                result.capCount += capCount;
                return result;
              });
        }
      }
      ZC_UNREACHABLE;
    }

    Promise<ReadResult> tryReadWithStreams(void* buffer, size_t minBytes, size_t maxBytes,
                                           Own<AsyncCapabilityStream>* streamBuffer,
                                           size_t maxStreams) override {
      size_t capCount = 0;
      {  // TODO(cleanup): Remove redundant braces when we update to C++17.
        ZC_SWITCH_ONEOF(capBuffer) {
          ZC_CASE_ONEOF(fds, ArrayPtr<const int>) {
            if (fds.size() > 0 && maxStreams > 0) {
              // TODO(someday): Use AsyncIoStream's `Maybe<int> getFd()` method?
              ZC_FAIL_REQUIRE(
                  "async pipe message was written with FDs attached, but corresponding read "
                  "asked for streams, and we don't know how to convert here");
            }
          }
          ZC_CASE_ONEOF(streams, Array<Own<AsyncCapabilityStream>>) {
            capCount = zc::max(streams.size(), maxStreams);
            for (auto i : zc::zeroTo(capCount)) { streamBuffer[i] = zc::mv(streams[i]); }
            streamBuffer += capCount;
            maxStreams -= capCount;
          }
        }
      }

      // Drop any unclaimed caps. This mirrors the behavior of unix sockets, where if we didn't
      // provide enough buffer space for all the written FDs, the remaining ones are lost.
      capBuffer = {};

      ZC_SWITCH_ONEOF(tryReadImpl(buffer, minBytes, maxBytes)) {
        ZC_CASE_ONEOF(done, Done) { return ReadResult{done.result, capCount}; }
        ZC_CASE_ONEOF(retry, Retry) {
          return pipe
              .tryReadWithStreams(retry.buffer, retry.minBytes, retry.maxBytes, streamBuffer,
                                  maxStreams)
              .then([byteCount = retry.alreadyRead, capCount](ReadResult result) {
                result.byteCount += byteCount;
                result.capCount += capCount;
                return result;
              });
        }
      }
      ZC_UNREACHABLE;
    }

    Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
      // Note: Pumps drop all capabilities.
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      if (amount < writeBuffer.size()) {
        // Consume a portion of the write buffer.
        return canceler.wrap(output.write(writeBuffer.first(amount))
                                 .then(
                                     [this, amount]() {
                                       writeBuffer = writeBuffer.slice(amount);
                                       // We pumped the full amount, so we're done pumping.
                                       return amount;
                                     },
                                     teeExceptionSize(fulfiller, canceler)));
      }

      // First piece doesn't cover the whole pump. Figure out how many more pieces to add.
      uint64_t actual = writeBuffer.size();
      size_t i = 0;
      while (i < morePieces.size() && amount >= actual + morePieces[i].size()) {
        actual += morePieces[i++].size();
      }

      // Write the first piece.
      auto promise = output.write(writeBuffer);

      // Write full pieces as a single gather-write.
      if (i > 0) {
        auto more = morePieces.first(i);
        promise = promise.then([&output, more]() { return output.write(more); });
      }

      if (i == morePieces.size()) {
        // This will complete the write.
        return canceler.wrap(promise.then(
            [this, &output, amount, actual]() -> Promise<uint64_t> {
              canceler.release();
              fulfiller.fulfill();
              pipe.endState(*this);

              if (actual == amount) {
                // Oh, we had exactly enough.
                return actual;
              } else {
                return pipe.pumpTo(output, amount - actual).then([actual](uint64_t actual2) {
                  return actual + actual2;
                });
              }
            },
            teeExceptionPromise<uint64_t>(fulfiller, canceler)));
      } else {
        // Pump ends mid-piece. Write the last, partial piece.
        auto n = amount - actual;
        auto splitPiece = morePieces[i];
        ZC_ASSERT(n <= splitPiece.size());
        auto newWriteBuffer = splitPiece.slice(n, splitPiece.size());
        auto newMorePieces = morePieces.slice(i + 1, morePieces.size());
        auto prefix = splitPiece.first(n);
        if (prefix.size() > 0) {
          promise = promise.then([&output, prefix]() { return output.write(prefix); });
        }

        return canceler.wrap(promise.then(
            [this, newWriteBuffer, newMorePieces, amount]() {
              writeBuffer = newWriteBuffer;
              morePieces = newMorePieces;
              canceler.release();
              return amount;
            },
            teeExceptionSize(fulfiller, canceler)));
      }
    }

    void abortRead() override {
      canceler.cancel("abortRead() was called");
      fulfiller.reject(ZC_EXCEPTION(DISCONNECTED, "read end of pipe was aborted"));
      pipe.endState(*this);
      pipe.abortRead();
    }

    Promise<void> write(ArrayPtr<const byte> buffer) override {
      ZC_FAIL_REQUIRE("can't write() again until previous write() completes");
    }
    Promise<void> write(ArrayPtr<const ArrayPtr<const byte>> pieces) override {
      ZC_FAIL_REQUIRE("can't write() again until previous write() completes");
    }
    Promise<void> writeWithFds(ArrayPtr<const byte> data,
                               ArrayPtr<const ArrayPtr<const byte>> moreData,
                               ArrayPtr<const int> fds) override {
      ZC_FAIL_REQUIRE("can't write() again until previous write() completes");
    }
    Promise<void> writeWithStreams(ArrayPtr<const byte> data,
                                   ArrayPtr<const ArrayPtr<const byte>> moreData,
                                   Array<Own<AsyncCapabilityStream>> streams) override {
      ZC_FAIL_REQUIRE("can't write() again until previous write() completes");
    }
    Maybe<Promise<uint64_t>> tryPumpFrom(AsyncInputStream& input, uint64_t amount) override {
      ZC_FAIL_REQUIRE("can't tryPumpFrom() again until previous write() completes");
    }
    void shutdownWrite() override {
      ZC_FAIL_REQUIRE("can't shutdownWrite() until previous write() completes");
    }

    Promise<void> whenWriteDisconnected() override {
      ZC_FAIL_ASSERT("can't get here -- implemented by AsyncPipe");
    }

  private:
    PromiseFulfiller<void>& fulfiller;
    AsyncPipe& pipe;
    ArrayPtr<const byte> writeBuffer;
    ArrayPtr<const ArrayPtr<const byte>> morePieces;
    zc::OneOf<ArrayPtr<const int>, Array<Own<AsyncCapabilityStream>>> capBuffer;
    Canceler canceler;

    struct Done {
      size_t result;
    };
    struct Retry {
      void* buffer;
      size_t minBytes;
      size_t maxBytes;
      size_t alreadyRead;
    };

    OneOf<Done, Retry> tryReadImpl(void* readBufferPtr, size_t minBytes, size_t maxBytes) {
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      auto readBuffer = arrayPtr(reinterpret_cast<byte*>(readBufferPtr), maxBytes);

      size_t totalRead = 0;
      while (readBuffer.size() >= writeBuffer.size()) {
        // The whole current write buffer can be copied into the read buffer.

        {
          auto n = writeBuffer.size();
          memcpy(readBuffer.begin(), writeBuffer.begin(), n);
          totalRead += n;
          readBuffer = readBuffer.slice(n, readBuffer.size());
        }

        if (morePieces.size() == 0) {
          // All done writing.
          fulfiller.fulfill();
          pipe.endState(*this);

          if (totalRead >= minBytes) {
            // Also all done reading.
            return Done{totalRead};
          } else {
            return Retry{readBuffer.begin(), minBytes - totalRead, readBuffer.size(), totalRead};
          }
        }

        writeBuffer = morePieces[0];
        morePieces = morePieces.slice(1, morePieces.size());
      }

      // At this point, the read buffer is smaller than the current write buffer, so we can fill
      // it completely.
      {
        auto n = readBuffer.size();
        memcpy(readBuffer.begin(), writeBuffer.begin(), n);
        writeBuffer = writeBuffer.slice(n, writeBuffer.size());
        totalRead += n;
      }

      return Done{totalRead};
    }
  };

  class BlockedPumpFrom final : public AsyncCapabilityStream {
    // AsyncPipe state when a tryPumpFrom() is currently waiting for a corresponding read().

  public:
    BlockedPumpFrom(PromiseFulfiller<uint64_t>& fulfiller, AsyncPipe& pipe, AsyncInputStream& input,
                    uint64_t amount)
        : fulfiller(fulfiller), pipe(pipe), input(input), amount(amount) {
      ZC_REQUIRE(pipe.state == zc::none);
      pipe.state = *this;
    }

    ~BlockedPumpFrom() noexcept(false) { pipe.endState(*this); }

    Promise<size_t> tryRead(void* readBuffer, size_t minBytes, size_t maxBytes) override {
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      auto pumpLeft = amount - pumpedSoFar;
      auto min = zc::min(pumpLeft, minBytes);
      auto max = zc::min(pumpLeft, maxBytes);
      return canceler.wrap(
          input.tryRead(readBuffer, min, max)
              .then(
                  [this, readBuffer, minBytes, maxBytes,
                   min](size_t actual) -> zc::Promise<size_t> {
                    canceler.release();
                    pumpedSoFar += actual;
                    ZC_ASSERT(pumpedSoFar <= amount);

                    if (pumpedSoFar == amount || actual < min) {
                      // Either we pumped all we wanted or we hit EOF.
                      fulfiller.fulfill(zc::cp(pumpedSoFar));
                      pipe.endState(*this);
                    }

                    if (actual >= minBytes) {
                      return actual;
                    } else {
                      return pipe
                          .tryRead(reinterpret_cast<byte*>(readBuffer) + actual, minBytes - actual,
                                   maxBytes - actual)
                          .then([actual](size_t actual2) { return actual + actual2; });
                    }
                    // This teeExceptionPromise can mask the original exception because it causes
                    // BlockedPumpFrom to be resolved and then destroyed which causes canceler to
                    // cancel.
                  },
                  teeExceptionPromise<size_t>(fulfiller, canceler)));
    }

    Promise<ReadResult> tryReadWithFds(void* readBuffer, size_t minBytes, size_t maxBytes,
                                       AutoCloseFd* fdBuffer, size_t maxFds) override {
      // Pumps drop all capabilities, so fall back to regular read. (We don't even know if the
      // destination is an AsyncCapabilityStream...)
      return tryRead(readBuffer, minBytes, maxBytes).then([](size_t n) {
        return ReadResult{n, 0};
      });
    }

    Promise<ReadResult> tryReadWithStreams(void* readBuffer, size_t minBytes, size_t maxBytes,
                                           Own<AsyncCapabilityStream>* streamBuffer,
                                           size_t maxStreams) override {
      // Pumps drop all capabilities, so fall back to regular read. (We don't even know if the
      // destination is an AsyncCapabilityStream...)
      return tryRead(readBuffer, minBytes, maxBytes).then([](size_t n) {
        return ReadResult{n, 0};
      });
    }

    Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount2) override {
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      auto n = zc::min(amount2, amount - pumpedSoFar);
      return canceler.wrap(input.pumpTo(output, n).then(
          [this, &output, amount2, n](uint64_t actual) -> Promise<uint64_t> {
            canceler.release();
            pumpedSoFar += actual;
            ZC_ASSERT(pumpedSoFar <= amount);
            if (pumpedSoFar == amount || actual < n) {
              // Either we pumped all we wanted or we hit EOF.
              fulfiller.fulfill(zc::cp(pumpedSoFar));
              pipe.endState(*this);
              return pipe.pumpTo(output, amount2 - actual).then([actual](uint64_t actual2) {
                return actual + actual2;
              });
            }

            // Completed entire pumpTo amount.
            ZC_ASSERT(actual == amount2);
            return amount2;
          },
          teeExceptionSize(fulfiller, canceler)));
    }

    void abortRead() override {
      canceler.cancel("abortRead() was called");

      // The input might have reached EOF, but we haven't detected it yet because we haven't tried
      // to read that far. If we had not optimized tryPumpFrom() and instead used the default
      // pumpTo() implementation, then the input would not have called write() again once it
      // reached EOF, and therefore the abortRead() on the other end would *not* propagate an
      // exception! We need the same behavior here. To that end, we need to detect if we're at EOF
      // by reading one last byte.
      checkEofTask = zc::evalNow([&]() {
        static char junk;
        return input.tryRead(&junk, 1, 1)
            .then([this](uint64_t n) {
              if (n == 0) {
                fulfiller.fulfill(zc::cp(pumpedSoFar));
              } else {
                fulfiller.reject(ZC_EXCEPTION(DISCONNECTED, "read end of pipe was aborted"));
              }
            })
            .eagerlyEvaluate([this](zc::Exception&& e) { fulfiller.reject(zc::mv(e)); });
      });

      pipe.endState(*this);
      pipe.abortRead();
    }

    Promise<void> write(ArrayPtr<const byte> buffer) override {
      ZC_FAIL_REQUIRE("can't write() again until previous tryPumpFrom() completes");
    }
    Promise<void> write(ArrayPtr<const ArrayPtr<const byte>> pieces) override {
      ZC_FAIL_REQUIRE("can't write() again until previous tryPumpFrom() completes");
    }
    Promise<void> writeWithFds(ArrayPtr<const byte> data,
                               ArrayPtr<const ArrayPtr<const byte>> moreData,
                               ArrayPtr<const int> fds) override {
      ZC_FAIL_REQUIRE("can't write() again until previous tryPumpFrom() completes");
    }
    Promise<void> writeWithStreams(ArrayPtr<const byte> data,
                                   ArrayPtr<const ArrayPtr<const byte>> moreData,
                                   Array<Own<AsyncCapabilityStream>> streams) override {
      ZC_FAIL_REQUIRE("can't write() again until previous tryPumpFrom() completes");
    }
    Maybe<Promise<uint64_t>> tryPumpFrom(AsyncInputStream& input, uint64_t amount) override {
      ZC_FAIL_REQUIRE("can't tryPumpFrom() again until previous tryPumpFrom() completes");
    }
    void shutdownWrite() override {
      ZC_FAIL_REQUIRE("can't shutdownWrite() until previous tryPumpFrom() completes");
    }

    Promise<void> whenWriteDisconnected() override {
      ZC_FAIL_ASSERT("can't get here -- implemented by AsyncPipe");
    }

  private:
    PromiseFulfiller<uint64_t>& fulfiller;
    AsyncPipe& pipe;
    AsyncInputStream& input;
    uint64_t amount;
    uint64_t pumpedSoFar = 0;
    Canceler canceler;
    zc::Promise<void> checkEofTask = nullptr;
  };

  class BlockedRead final : public AsyncCapabilityStream {
    // AsyncPipe state when a tryRead() is currently waiting for a corresponding write().

  public:
    BlockedRead(
        PromiseFulfiller<ReadResult>& fulfiller, AsyncPipe& pipe, ArrayPtr<byte> readBuffer,
        size_t minBytes,
        zc::OneOf<ArrayPtr<AutoCloseFd>, ArrayPtr<Own<AsyncCapabilityStream>>> capBuffer = {})
        : fulfiller(fulfiller),
          pipe(pipe),
          readBuffer(readBuffer),
          minBytes(minBytes),
          capBuffer(capBuffer) {
      ZC_REQUIRE(pipe.state == zc::none);
      pipe.state = *this;
    }

    ~BlockedRead() noexcept(false) { pipe.endState(*this); }

    Promise<size_t> tryRead(void* readBuffer, size_t minBytes, size_t maxBytes) override {
      ZC_FAIL_REQUIRE("can't read() again until previous read() completes");
    }
    Promise<ReadResult> tryReadWithFds(void* readBuffer, size_t minBytes, size_t maxBytes,
                                       AutoCloseFd* fdBuffer, size_t maxFds) override {
      ZC_FAIL_REQUIRE("can't read() again until previous read() completes");
    }
    Promise<ReadResult> tryReadWithStreams(void* readBuffer, size_t minBytes, size_t maxBytes,
                                           Own<AsyncCapabilityStream>* streamBuffer,
                                           size_t maxStreams) override {
      ZC_FAIL_REQUIRE("can't read() again until previous read() completes");
    }
    Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
      ZC_FAIL_REQUIRE("can't read() again until previous read() completes");
    }

    void abortRead() override {
      canceler.cancel("abortRead() was called");
      fulfiller.reject(ZC_EXCEPTION(DISCONNECTED, "read end of pipe was aborted"));
      pipe.endState(*this);
      pipe.abortRead();
    }

    Promise<void> write(ArrayPtr<const byte> buffer) override {
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      ZC_SWITCH_ONEOF(writeImpl(buffer, nullptr)) {
        ZC_CASE_ONEOF(done, Done) { return READY_NOW; }
        ZC_CASE_ONEOF(retry, Retry) {
          ZC_ASSERT(retry.moreData == nullptr);
          return pipe.write(retry.data);
        }
      }
      ZC_UNREACHABLE;
    }

    Promise<void> write(ArrayPtr<const ArrayPtr<const byte>> pieces) override {
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      ZC_SWITCH_ONEOF(writeImpl(pieces[0], pieces.slice(1, pieces.size()))) {
        ZC_CASE_ONEOF(done, Done) { return READY_NOW; }
        ZC_CASE_ONEOF(retry, Retry) {
          if (retry.data.size() == 0) {
            // We exactly finished the current piece, so just issue a write for the remaining
            // pieces.
            if (retry.moreData.size() == 0) {
              // Nothing left.
              return READY_NOW;
            } else {
              // Write remaining pieces.
              return pipe.write(retry.moreData);
            }
          } else {
            // Unfortunately we have to execute a separate write() for the remaining part of this
            // piece, because we can't modify the pieces array.
            auto promise = pipe.write(retry.data);
            if (retry.moreData.size() == 0) {
              // No more pieces so that's it.
              return zc::mv(promise);
            } else {
              // Also need to write the remaining pieces.
              auto& pipeRef = pipe;
              return promise.then(
                  [pieces = retry.moreData, &pipeRef]() { return pipeRef.write(pieces); });
            }
          }
        }
      }
      ZC_UNREACHABLE;
    }

    Promise<void> writeWithFds(ArrayPtr<const byte> data,
                               ArrayPtr<const ArrayPtr<const byte>> moreData,
                               ArrayPtr<const int> fds) override {
#if __GNUC__ && !__clang__ && __GNUC__ >= 7
// GCC 7 decides the open-brace below is "misleadingly indented" as if it were guarded by the `for`
// that appears in the implementation of ZC_REQUIRE(). Shut up shut up shut up.
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      {  // TODO(cleanup): Remove redundant braces when we update to C++17.
        ZC_SWITCH_ONEOF(capBuffer) {
          ZC_CASE_ONEOF(fdBuffer, ArrayPtr<AutoCloseFd>) {
            size_t count = zc::max(fdBuffer.size(), fds.size());
            // Unfortunately, we have to dup() each FD, because the writer doesn't release ownership
            // by default.
            // TODO(perf): Should we add an ownership-releasing version of writeWithFds()?
            for (auto i : zc::zeroTo(count)) {
              int duped;
              ZC_SYSCALL(duped = dup(fds[i]));
              fdBuffer[i] = zc::AutoCloseFd(duped);
            }
            capBuffer = fdBuffer.slice(count, fdBuffer.size());
            readSoFar.capCount += count;
          }
          ZC_CASE_ONEOF(streamBuffer, ArrayPtr<Own<AsyncCapabilityStream>>) {
            if (streamBuffer.size() > 0 && fds.size() > 0) {
              // TODO(someday): Use AsyncIoStream's `Maybe<int> getFd()` method?
              ZC_FAIL_REQUIRE(
                  "async pipe message was written with FDs attached, but corresponding read "
                  "asked for streams, and we don't know how to convert here");
            }
          }
        }
      }

      ZC_SWITCH_ONEOF(writeImpl(data, moreData)) {
        ZC_CASE_ONEOF(done, Done) { return READY_NOW; }
        ZC_CASE_ONEOF(retry, Retry) {
          // Any leftover fds in `fds` are dropped on the floor, per contract.
          // TODO(cleanup): We use another writeWithFds() call here only because it accepts `data`
          //   and `moreData` directly. After the stream API refactor, we should be able to avoid
          //   this.
          return pipe.writeWithFds(retry.data, retry.moreData, nullptr);
        }
      }
      ZC_UNREACHABLE;
    }

    Promise<void> writeWithStreams(ArrayPtr<const byte> data,
                                   ArrayPtr<const ArrayPtr<const byte>> moreData,
                                   Array<Own<AsyncCapabilityStream>> streams) override {
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      {  // TODO(cleanup): Remove redundant braces when we update to C++17.
        ZC_SWITCH_ONEOF(capBuffer) {
          ZC_CASE_ONEOF(fdBuffer, ArrayPtr<AutoCloseFd>) {
            if (fdBuffer.size() > 0 && streams.size() > 0) {
              // TODO(someday): We could let people pass a LowLevelAsyncIoProvider to
              // newTwoWayPipe()
              //   if we wanted to auto-wrap FDs, but does anyone care?
              ZC_FAIL_REQUIRE(
                  "async pipe message was written with streams attached, but corresponding read "
                  "asked for FDs, and we don't know how to convert here");
            }
          }
          ZC_CASE_ONEOF(streamBuffer, ArrayPtr<Own<AsyncCapabilityStream>>) {
            size_t count = zc::max(streamBuffer.size(), streams.size());
            for (auto i : zc::zeroTo(count)) { streamBuffer[i] = zc::mv(streams[i]); }
            capBuffer = streamBuffer.slice(count, streamBuffer.size());
            readSoFar.capCount += count;
          }
        }
      }

      ZC_SWITCH_ONEOF(writeImpl(data, moreData)) {
        ZC_CASE_ONEOF(done, Done) { return READY_NOW; }
        ZC_CASE_ONEOF(retry, Retry) {
          // Any leftover fds in `fds` are dropped on the floor, per contract.
          // TODO(cleanup): We use another writeWithStreams() call here only because it accepts
          //   `data` and `moreData` directly. After the stream API refactor, we should be able to
          //   avoid this.
          return pipe.writeWithStreams(retry.data, retry.moreData, nullptr);
        }
      }
      ZC_UNREACHABLE;
    }

    Maybe<Promise<uint64_t>> tryPumpFrom(AsyncInputStream& input, uint64_t amount) override {
      // Note: Pumps drop all capabilities.
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      ZC_ASSERT(minBytes > readSoFar.byteCount);
      auto minToRead = zc::min(amount, minBytes - readSoFar.byteCount);
      auto maxToRead = zc::min(amount, readBuffer.size());

      return canceler.wrap(input.tryRead(readBuffer.begin(), minToRead, maxToRead)
                               .then(
                                   [this, &input, amount](size_t actual) -> Promise<uint64_t> {
                                     readBuffer = readBuffer.slice(actual, readBuffer.size());
                                     readSoFar.byteCount += actual;

                                     if (readSoFar.byteCount >= minBytes) {
                                       // We've read enough to close out this read (readSoFar >=
                                       // minBytes).
                                       canceler.release();
                                       fulfiller.fulfill(zc::cp(readSoFar));
                                       pipe.endState(*this);

                                       if (actual < amount) {
                                         // We didn't read as much data as the pump requested, but
                                         // we did fulfill the read, so we don't know whether we
                                         // reached EOF on the input. We need to continue the pump,
                                         // replacing the BlockedRead state.
                                         return input.pumpTo(pipe, amount - actual)
                                             .then([actual](uint64_t actual2) -> uint64_t {
                                               return actual + actual2;
                                             });
                                       } else {
                                         // We pumped as much data as was requested, so we can
                                         // return that now.
                                         return actual;
                                       }
                                     } else {
                                       // The pump completed without fulfilling the read. This
                                       // either means that the pump reached EOF or the `amount`
                                       // requested was not enough to satisfy the read in the first
                                       // place. Pumps do not propagate EOF, so either way we want
                                       // to leave the BlockedRead in place waiting for more data.
                                       return actual;
                                     }
                                   },
                                   teeExceptionPromise<uint64_t>(fulfiller, canceler)));
    }

    void shutdownWrite() override {
      canceler.cancel("shutdownWrite() was called");
      fulfiller.fulfill(zc::cp(readSoFar));
      pipe.endState(*this);
      pipe.shutdownWrite();
    }

    Promise<void> whenWriteDisconnected() override {
      ZC_FAIL_ASSERT("can't get here -- implemented by AsyncPipe");
    }

  private:
    PromiseFulfiller<ReadResult>& fulfiller;
    AsyncPipe& pipe;
    ArrayPtr<byte> readBuffer;
    size_t minBytes;
    zc::OneOf<ArrayPtr<AutoCloseFd>, ArrayPtr<Own<AsyncCapabilityStream>>> capBuffer;
    ReadResult readSoFar = {0, 0};
    Canceler canceler;

    struct Done {};
    struct Retry {
      ArrayPtr<const byte> data;
      ArrayPtr<const ArrayPtr<const byte>> moreData;
    };

    OneOf<Done, Retry> writeImpl(ArrayPtr<const byte> data,
                                 ArrayPtr<const ArrayPtr<const byte>> moreData) {
      for (;;) {
        if (data.size() < readBuffer.size()) {
          // First write segment consumes a portion of the read buffer but not all of it.
          auto n = data.size();
          memcpy(readBuffer.begin(), data.begin(), n);
          readSoFar.byteCount += n;
          readBuffer = readBuffer.slice(n, readBuffer.size());
          if (moreData.size() == 0) {
            // Consumed all written pieces.
            if (readSoFar.byteCount >= minBytes) {
              // We've read enough to close out this read.
              fulfiller.fulfill(zc::cp(readSoFar));
              pipe.endState(*this);
            }
            return Done();
          }
          data = moreData[0];
          moreData = moreData.slice(1, moreData.size());
          // loop
        } else {
          // First write segment consumes entire read buffer.
          auto n = readBuffer.size();
          readSoFar.byteCount += n;
          fulfiller.fulfill(zc::cp(readSoFar));
          pipe.endState(*this);
          memcpy(readBuffer.begin(), data.begin(), n);

          data = data.slice(n, data.size());
          if (data.size() == 0 && moreData.size() == 0) {
            return Done();
          } else {
            // Note: Even if `data` is empty, we don't replace it with moreData[0], because the
            //   retry might need to use write(ArrayPtr<ArrayPtr<byte>>) which doesn't allow
            //   passing a separate first segment.
            return Retry{data, moreData};
          }
        }
      }
    }
  };

  class BlockedPumpTo final : public AsyncCapabilityStream {
    // AsyncPipe state when a pumpTo() is currently waiting for a corresponding write().

  public:
    BlockedPumpTo(PromiseFulfiller<uint64_t>& fulfiller, AsyncPipe& pipe, AsyncOutputStream& output,
                  uint64_t amount)
        : fulfiller(fulfiller), pipe(pipe), output(output), amount(amount) {
      ZC_REQUIRE(pipe.state == zc::none);
      pipe.state = *this;
    }

    ~BlockedPumpTo() noexcept(false) { pipe.endState(*this); }

    Promise<size_t> tryRead(void* readBuffer, size_t minBytes, size_t maxBytes) override {
      ZC_FAIL_REQUIRE("can't read() again until previous pumpTo() completes");
    }
    Promise<ReadResult> tryReadWithFds(void* readBuffer, size_t minBytes, size_t maxBytes,
                                       AutoCloseFd* fdBuffer, size_t maxFds) override {
      ZC_FAIL_REQUIRE("can't read() again until previous pumpTo() completes");
    }
    Promise<ReadResult> tryReadWithStreams(void* readBuffer, size_t minBytes, size_t maxBytes,
                                           Own<AsyncCapabilityStream>* streamBuffer,
                                           size_t maxStreams) override {
      ZC_FAIL_REQUIRE("can't read() again until previous pumpTo() completes");
    }
    Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
      ZC_FAIL_REQUIRE("can't read() again until previous pumpTo() completes");
    }

    void abortRead() override {
      canceler.cancel("abortRead() was called");
      fulfiller.reject(ZC_EXCEPTION(DISCONNECTED, "read end of pipe was aborted"));
      pipe.endState(*this);
      pipe.abortRead();
    }

    Promise<void> write(ArrayPtr<const byte> writeBuffer) override {
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      auto actual = zc::min(amount - pumpedSoFar, writeBuffer.size());
      return canceler.wrap(output.write(writeBuffer.first(actual))
                               .then(
                                   [this, actual, writeBuffer]() -> zc::Promise<void> {
                                     canceler.release();
                                     pumpedSoFar += actual;

                                     ZC_ASSERT(pumpedSoFar <= amount);
                                     ZC_ASSERT(actual <= writeBuffer.size());

                                     if (pumpedSoFar == amount) {
                                       // Done with pump.
                                       fulfiller.fulfill(zc::cp(pumpedSoFar));
                                       pipe.endState(*this);
                                     }

                                     if (actual == writeBuffer.size()) {
                                       return zc::READY_NOW;
                                     } else {
                                       ZC_ASSERT(pumpedSoFar == amount);
                                       return pipe.write(writeBuffer.slice(actual));
                                     }
                                   },
                                   teeExceptionPromise<void>(fulfiller, canceler)));
    }

    Promise<void> write(ArrayPtr<const ArrayPtr<const byte>> pieces) override {
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      size_t size = 0;
      size_t needed = amount - pumpedSoFar;
      for (auto i : zc::indices(pieces)) {
        if (pieces[i].size() > needed) {
          // The pump ends in the middle of this write.

          auto promise = output.write(pieces.first(i));

          if (needed > 0) {
            // The pump includes part of this piece, but not all. Unfortunately we need to split
            // writes.
            auto partial = pieces[i].first(needed);
            promise = promise.then([this, partial]() { return output.write(partial); });
            auto partial2 = pieces[i].slice(needed, pieces[i].size());
            promise = canceler.wrap(promise.then(
                [this, partial2]() {
                  canceler.release();
                  fulfiller.fulfill(zc::cp(amount));
                  pipe.endState(*this);
                  return pipe.write(partial2);
                },
                teeExceptionPromise<void>(fulfiller, canceler)));
            ++i;
          } else {
            // The pump ends exactly at the end of a piece, how nice.
            promise = canceler.wrap(promise.then(
                [this]() {
                  canceler.release();
                  fulfiller.fulfill(zc::cp(amount));
                  pipe.endState(*this);
                },
                teeExceptionVoid(fulfiller, canceler)));
          }

          auto remainder = pieces.slice(i, pieces.size());
          if (remainder.size() > 0) {
            auto& pipeRef = pipe;
            promise = promise.then([&pipeRef, remainder]() { return pipeRef.write(remainder); });
          }

          return promise;
        } else {
          size += pieces[i].size();
          needed -= pieces[i].size();
        }
      }

      // Turns out we can forward this whole write.
      ZC_ASSERT(size <= amount - pumpedSoFar);
      return canceler.wrap(output.write(pieces).then(
          [this, size]() {
            pumpedSoFar += size;
            ZC_ASSERT(pumpedSoFar <= amount);
            if (pumpedSoFar == amount) {
              // Done pumping.
              canceler.release();
              fulfiller.fulfill(zc::cp(amount));
              pipe.endState(*this);
            }
          },
          teeExceptionVoid(fulfiller, canceler)));
    }

    Promise<void> writeWithFds(ArrayPtr<const byte> data,
                               ArrayPtr<const ArrayPtr<const byte>> moreData,
                               ArrayPtr<const int> fds) override {
      // Pumps drop all capabilities, so fall back to regular write().

      // TODO(cleaunp): After stream API refactor, regular write() methods will take
      //   (data, moreData) and we can clean this up.
      if (moreData.size() == 0) {
        return write(data);
      } else {
        auto pieces = zc::heapArrayBuilder<const ArrayPtr<const byte>>(moreData.size() + 1);
        pieces.add(data);
        pieces.addAll(moreData);
        return write(pieces.finish());
      }
    }

    Promise<void> writeWithStreams(ArrayPtr<const byte> data,
                                   ArrayPtr<const ArrayPtr<const byte>> moreData,
                                   Array<Own<AsyncCapabilityStream>> streams) override {
      // Pumps drop all capabilities, so fall back to regular write().

      // TODO(cleaunp): After stream API refactor, regular write() methods will take
      //   (data, moreData) and we can clean this up.
      if (moreData.size() == 0) {
        return write(data);
      } else {
        auto pieces = zc::heapArrayBuilder<const ArrayPtr<const byte>>(moreData.size() + 1);
        pieces.add(data);
        pieces.addAll(moreData);
        return write(pieces.finish());
      }
    }

    Maybe<Promise<uint64_t>> tryPumpFrom(AsyncInputStream& input, uint64_t amount2) override {
      ZC_REQUIRE(canceler.isEmpty(), "already pumping");

      auto n = zc::min(amount2, amount - pumpedSoFar);
      return output.tryPumpFrom(input, n).map([&](Promise<uint64_t> subPump) {
        return canceler.wrap(subPump.then(
            [this, &input, amount2, n](uint64_t actual) -> Promise<uint64_t> {
              canceler.release();
              pumpedSoFar += actual;
              ZC_ASSERT(pumpedSoFar <= amount);
              if (pumpedSoFar == amount) {
                fulfiller.fulfill(zc::cp(amount));
                pipe.endState(*this);
              }

              ZC_ASSERT(actual <= amount2);
              if (actual == amount2) {
                // Completed entire tryPumpFrom amount.
                return amount2;
              } else if (actual < n) {
                // Received less than requested, presumably because EOF.
                return actual;
              } else {
                // We received all the bytes that were requested but it didn't complete the pump.
                ZC_ASSERT(pumpedSoFar == amount);
                return input.pumpTo(pipe, amount2 - actual);
              }
            },
            teeExceptionPromise<uint64_t>(fulfiller, canceler)));
      });
    }

    void shutdownWrite() override {
      canceler.cancel("shutdownWrite() was called");
      fulfiller.fulfill(zc::cp(pumpedSoFar));
      pipe.endState(*this);
      pipe.shutdownWrite();
    }

    Promise<void> whenWriteDisconnected() override {
      ZC_FAIL_ASSERT("can't get here -- implemented by AsyncPipe");
    }

  private:
    PromiseFulfiller<uint64_t>& fulfiller;
    AsyncPipe& pipe;
    AsyncOutputStream& output;
    uint64_t amount;
    size_t pumpedSoFar = 0;
    Canceler canceler;
  };

  class AbortedRead final : public AsyncCapabilityStream {
    // AsyncPipe state when abortRead() has been called.

  public:
    Promise<size_t> tryRead(void* readBufferPtr, size_t minBytes, size_t maxBytes) override {
      return ZC_EXCEPTION(DISCONNECTED, "abortRead() has been called");
    }
    Promise<ReadResult> tryReadWithFds(void* readBuffer, size_t minBytes, size_t maxBytes,
                                       AutoCloseFd* fdBuffer, size_t maxFds) override {
      return ZC_EXCEPTION(DISCONNECTED, "abortRead() has been called");
    }
    Promise<ReadResult> tryReadWithStreams(void* readBuffer, size_t minBytes, size_t maxBytes,
                                           Own<AsyncCapabilityStream>* streamBuffer,
                                           size_t maxStreams) override {
      return ZC_EXCEPTION(DISCONNECTED, "abortRead() has been called");
    }
    Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
      return ZC_EXCEPTION(DISCONNECTED, "abortRead() has been called");
    }
    void abortRead() override {
      // ignore repeated abort
    }

    Promise<void> write(ArrayPtr<const byte> buffer) override {
      return ZC_EXCEPTION(DISCONNECTED, "abortRead() has been called");
    }
    Promise<void> write(ArrayPtr<const ArrayPtr<const byte>> pieces) override {
      return ZC_EXCEPTION(DISCONNECTED, "abortRead() has been called");
    }
    Promise<void> writeWithFds(ArrayPtr<const byte> data,
                               ArrayPtr<const ArrayPtr<const byte>> moreData,
                               ArrayPtr<const int> fds) override {
      return ZC_EXCEPTION(DISCONNECTED, "abortRead() has been called");
    }
    Promise<void> writeWithStreams(ArrayPtr<const byte> data,
                                   ArrayPtr<const ArrayPtr<const byte>> moreData,
                                   Array<Own<AsyncCapabilityStream>> streams) override {
      return ZC_EXCEPTION(DISCONNECTED, "abortRead() has been called");
    }
    Maybe<Promise<uint64_t>> tryPumpFrom(AsyncInputStream& input, uint64_t amount) override {
      // There might not actually be any data in `input`, in which case a pump wouldn't actually
      // write anything and wouldn't fail.

      if (input.tryGetLength().orDefault(1) == 0) {
        // Yeah a pump would pump nothing.
        return constPromise<uint64_t, 0>();
      } else {
        // While we *could* just return zc::none here, it would probably then fall back to a normal
        // buffered pump, which would allocate a big old buffer just to find there's nothing to
        // read. Let's try reading 1 byte to avoid that allocation.
        static char c;
        return input.tryRead(&c, 1, 1).then([](size_t n) {
          if (n == 0) {
            // Yay, we're at EOF as hoped.
            return uint64_t(0);
          } else {
            // There was data in the input. The pump would have thrown.
            zc::throwRecoverableException(
                ZC_EXCEPTION(DISCONNECTED, "abortRead() has been called"));
            return uint64_t(0);
          }
        });
      }
    }
    void shutdownWrite() override {
      // ignore -- currently shutdownWrite() actually means that the PipeWriteEnd was dropped,
      // which is not an error even if reads have been aborted.
    }
    Promise<void> whenWriteDisconnected() override {
      ZC_FAIL_ASSERT("can't get here -- implemented by AsyncPipe");
    }
  };

  class ShutdownedWrite final : public AsyncCapabilityStream {
    // AsyncPipe state when shutdownWrite() has been called.

  public:
    Promise<size_t> tryRead(void* readBufferPtr, size_t minBytes, size_t maxBytes) override {
      return constPromise<size_t, 0>();
    }
    Promise<ReadResult> tryReadWithFds(void* readBuffer, size_t minBytes, size_t maxBytes,
                                       AutoCloseFd* fdBuffer, size_t maxFds) override {
      return ReadResult{0, 0};
    }
    Promise<ReadResult> tryReadWithStreams(void* readBuffer, size_t minBytes, size_t maxBytes,
                                           Own<AsyncCapabilityStream>* streamBuffer,
                                           size_t maxStreams) override {
      return ReadResult{0, 0};
    }
    Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
      return constPromise<uint64_t, 0>();
    }
    void abortRead() override {
      // ignore
    }

    Promise<void> write(ArrayPtr<const byte> buffer) override {
      ZC_FAIL_REQUIRE("shutdownWrite() has been called");
    }
    Promise<void> write(ArrayPtr<const ArrayPtr<const byte>> pieces) override {
      ZC_FAIL_REQUIRE("shutdownWrite() has been called");
    }
    Promise<void> writeWithFds(ArrayPtr<const byte> data,
                               ArrayPtr<const ArrayPtr<const byte>> moreData,
                               ArrayPtr<const int> fds) override {
      ZC_FAIL_REQUIRE("shutdownWrite() has been called");
    }
    Promise<void> writeWithStreams(ArrayPtr<const byte> data,
                                   ArrayPtr<const ArrayPtr<const byte>> moreData,
                                   Array<Own<AsyncCapabilityStream>> streams) override {
      ZC_FAIL_REQUIRE("shutdownWrite() has been called");
    }
    Maybe<Promise<uint64_t>> tryPumpFrom(AsyncInputStream& input, uint64_t amount) override {
      ZC_FAIL_REQUIRE("shutdownWrite() has been called");
    }
    void shutdownWrite() override {
      // ignore -- currently shutdownWrite() actually means that the PipeWriteEnd was dropped,
      // so it will only be called once anyhow.
    }
    Promise<void> whenWriteDisconnected() override {
      ZC_FAIL_ASSERT("can't get here -- implemented by AsyncPipe");
    }
  };
};

class PipeReadEnd final : public AsyncInputStream {
public:
  PipeReadEnd(zc::Own<AsyncPipe> pipe) : pipe(zc::mv(pipe)) {}
  ~PipeReadEnd() noexcept(false) {
    unwind.catchExceptionsIfUnwinding([&]() { pipe->abortRead(); });
  }

  Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return pipe->tryRead(buffer, minBytes, maxBytes);
  }

  Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
    return pipe->pumpTo(output, amount);
  }

private:
  Own<AsyncPipe> pipe;
  UnwindDetector unwind;
};

class PipeWriteEnd final : public AsyncOutputStream {
public:
  PipeWriteEnd(zc::Own<AsyncPipe> pipe) : pipe(zc::mv(pipe)) {}
  ~PipeWriteEnd() noexcept(false) {
    unwind.catchExceptionsIfUnwinding([&]() { pipe->shutdownWrite(); });
  }

  Promise<void> write(ArrayPtr<const byte> buffer) override { return pipe->write(buffer); }

  Promise<void> write(ArrayPtr<const ArrayPtr<const byte>> pieces) override {
    return pipe->write(pieces);
  }

  Maybe<Promise<uint64_t>> tryPumpFrom(AsyncInputStream& input, uint64_t amount) override {
    return pipe->tryPumpFrom(input, amount);
  }

  Promise<void> whenWriteDisconnected() override { return pipe->whenWriteDisconnected(); }

private:
  Own<AsyncPipe> pipe;
  UnwindDetector unwind;
};

class TwoWayPipeEnd final : public AsyncCapabilityStream {
public:
  TwoWayPipeEnd(zc::Own<AsyncPipe> in, zc::Own<AsyncPipe> out) : in(zc::mv(in)), out(zc::mv(out)) {}
  ~TwoWayPipeEnd() noexcept(false) {
    unwind.catchExceptionsIfUnwinding([&]() {
      out->shutdownWrite();
      in->abortRead();
    });
  }

  Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return in->tryRead(buffer, minBytes, maxBytes);
  }
  Promise<ReadResult> tryReadWithFds(void* buffer, size_t minBytes, size_t maxBytes,
                                     AutoCloseFd* fdBuffer, size_t maxFds) override {
    return in->tryReadWithFds(buffer, minBytes, maxBytes, fdBuffer, maxFds);
  }
  Promise<ReadResult> tryReadWithStreams(void* buffer, size_t minBytes, size_t maxBytes,
                                         Own<AsyncCapabilityStream>* streamBuffer,
                                         size_t maxStreams) override {
    return in->tryReadWithStreams(buffer, minBytes, maxBytes, streamBuffer, maxStreams);
  }
  Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
    return in->pumpTo(output, amount);
  }
  void abortRead() override { in->abortRead(); }

  Promise<void> write(ArrayPtr<const byte> buffer) override { return out->write(buffer); }
  Promise<void> write(ArrayPtr<const ArrayPtr<const byte>> pieces) override {
    return out->write(pieces);
  }
  Promise<void> writeWithFds(ArrayPtr<const byte> data,
                             ArrayPtr<const ArrayPtr<const byte>> moreData,
                             ArrayPtr<const int> fds) override {
    return out->writeWithFds(data, moreData, fds);
  }
  Promise<void> writeWithStreams(ArrayPtr<const byte> data,
                                 ArrayPtr<const ArrayPtr<const byte>> moreData,
                                 Array<Own<AsyncCapabilityStream>> streams) override {
    return out->writeWithStreams(data, moreData, zc::mv(streams));
  }
  Maybe<Promise<uint64_t>> tryPumpFrom(AsyncInputStream& input, uint64_t amount) override {
    return out->tryPumpFrom(input, amount);
  }
  Promise<void> whenWriteDisconnected() override { return out->whenWriteDisconnected(); }
  void shutdownWrite() override { out->shutdownWrite(); }

private:
  zc::Own<AsyncPipe> in;
  zc::Own<AsyncPipe> out;
  UnwindDetector unwind;
};

class LimitedInputStream final : public AsyncInputStream {
public:
  LimitedInputStream(zc::Own<AsyncInputStream> inner, uint64_t limit)
      : inner(zc::mv(inner)), limit(limit) {
    if (limit == 0) { this->inner = nullptr; }
  }

  Maybe<uint64_t> tryGetLength() override { return limit; }

  Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    if (limit == 0) return constPromise<size_t, 0>();
    return inner->tryRead(buffer, zc::min(minBytes, limit), zc::min(maxBytes, limit))
        .then([this, minBytes](size_t actual) {
          decreaseLimit(actual, minBytes);
          return actual;
        });
  }

  Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
    if (limit == 0) return constPromise<uint64_t, 0>();
    auto requested = zc::min(amount, limit);
    return inner->pumpTo(output, requested).then([this, requested](uint64_t actual) {
      decreaseLimit(actual, requested);
      return actual;
    });
  }

private:
  Own<AsyncInputStream> inner;
  uint64_t limit;

  void decreaseLimit(uint64_t amount, uint64_t requested) {
    ZC_ASSERT(limit >= amount);
    limit -= amount;
    if (limit == 0) {
      inner = nullptr;
    } else if (amount < requested) {
      zc::throwRecoverableException(
          ZC_EXCEPTION(DISCONNECTED, "fixed-length pipe ended prematurely"));
    }
  }
};

}  // namespace

OneWayPipe newOneWayPipe(zc::Maybe<uint64_t> expectedLength) {
  auto impl = zc::refcounted<AsyncPipe>();
  Own<AsyncInputStream> readEnd = zc::heap<PipeReadEnd>(zc::addRef(*impl));
  ZC_IF_SOME(l, expectedLength) { readEnd = zc::heap<LimitedInputStream>(zc::mv(readEnd), l); }
  Own<AsyncOutputStream> writeEnd = zc::heap<PipeWriteEnd>(zc::mv(impl));
  return {zc::mv(readEnd), zc::mv(writeEnd)};
}

TwoWayPipe newTwoWayPipe() {
  auto pipe1 = zc::refcounted<AsyncPipe>();
  auto pipe2 = zc::refcounted<AsyncPipe>();
  auto end1 = zc::heap<TwoWayPipeEnd>(zc::addRef(*pipe1), zc::addRef(*pipe2));
  auto end2 = zc::heap<TwoWayPipeEnd>(zc::mv(pipe2), zc::mv(pipe1));
  return {{zc::mv(end1), zc::mv(end2)}};
}

CapabilityPipe newCapabilityPipe() {
  auto pipe1 = zc::refcounted<AsyncPipe>();
  auto pipe2 = zc::refcounted<AsyncPipe>();
  auto end1 = zc::heap<TwoWayPipeEnd>(zc::addRef(*pipe1), zc::addRef(*pipe2));
  auto end2 = zc::heap<TwoWayPipeEnd>(zc::mv(pipe2), zc::mv(pipe1));
  return {{zc::mv(end1), zc::mv(end2)}};
}

namespace {

class AsyncTee final : public Refcounted {
  class Buffer {
  public:
    Buffer() = default;

    uint64_t consume(ArrayPtr<byte>& readBuffer, size_t& minBytes);
    // Consume as many bytes as possible, copying them into `readBuffer`. Return the number of bytes
    // consumed.
    //
    // `readBuffer` and `minBytes` are both assigned appropriate new values, such that after any
    // call to `consume()`, `readBuffer` will point to the remaining slice of unwritten space, and
    // `minBytes` will have been decremented (clamped to zero) by the amount of bytes read. That is,
    // the read can be considered fulfilled if `minBytes` is zero after a call to `consume()`.

    Array<const ArrayPtr<const byte>> asArray(uint64_t minBytes, uint64_t& amount);
    // Consume the first `minBytes` of the buffer (or the entire buffer) and return it in an Array
    // of ArrayPtr<const byte>s, suitable for passing to AsyncOutputStream.write(). The outer Array
    // owns the underlying data.

    void produce(Array<byte> bytes);
    // Enqueue a byte array to the end of the buffer list.

    bool empty() const;
    uint64_t size() const;

    Buffer clone() const {
      size_t size = 0;
      for (const auto& buf : bufferList) { size += buf.size(); }
      auto builder = heapArrayBuilder<byte>(size);
      for (const auto& buf : bufferList) { builder.addAll(buf); }
      std::deque<Array<byte>> deque;
      deque.emplace_back(builder.finish());
      return Buffer{mv(deque)};
    }

  private:
    Buffer(std::deque<Array<byte>>&& buffer) : bufferList(mv(buffer)) {}

    std::deque<Array<byte>> bufferList;
  };

  class Sink;

public:
  class Branch final : public AsyncInputStream {
  public:
    Branch(Own<AsyncTee> teeArg) : tee(mv(teeArg)) { tee->branches.add(*this); }

    Branch(Own<AsyncTee> teeArg, Branch& cloneFrom)
        : tee(mv(teeArg)), buffer(cloneFrom.buffer.clone()) {
      tee->branches.add(*this);
    }

    ~Branch() noexcept(false) {
      ZC_ASSERT(link.isLinked()) {
        // Don't std::terminate().
        return;
      }
      tee->branches.remove(*this);

      ZC_REQUIRE(
          sink == zc::none,
          "destroying tee branch with operation still in-progress; probably going to segfault") {
        // Don't std::terminate().
        break;
      }
    }

    Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
      return tee->tryRead(*this, buffer, minBytes, maxBytes);
    }

    Promise<uint64_t> pumpTo(AsyncOutputStream& output, uint64_t amount) override {
      return tee->pumpTo(*this, output, amount);
    }

    Maybe<uint64_t> tryGetLength() override { return tee->tryGetLength(*this); }

    Maybe<Own<AsyncInputStream>> tryTee(uint64_t limit) override {
      if (tee->getBufferSizeLimit() != limit) {
        // Cannot optimize this path as the limit has changed, so we need a new AsyncTee to manage
        // the limit.
        return zc::none;
      }

      return zc::heap<Branch>(addRef(*tee), *this);
    }

  private:
    Own<AsyncTee> tee;
    ListLink<Branch> link;

    Buffer buffer;
    Maybe<Sink&> sink;

    friend class AsyncTee;
  };

  explicit AsyncTee(Own<AsyncInputStream> inner, uint64_t bufferSizeLimit)
      : inner(mv(inner)), bufferSizeLimit(bufferSizeLimit), length(this->inner->tryGetLength()) {}
  ~AsyncTee() noexcept(false) {
    ZC_ASSERT(branches.size() == 0, "destroying AsyncTee with branch still alive") {
      // Don't std::terminate().
      break;
    }
  }

  Promise<size_t> tryRead(Branch& branch, void* buffer, size_t minBytes, size_t maxBytes) {
    ZC_ASSERT(branch.sink == zc::none);

    // If there is excess data in the buffer for us, slurp that up.
    auto readBuffer = arrayPtr(reinterpret_cast<byte*>(buffer), maxBytes);
    auto readSoFar = branch.buffer.consume(readBuffer, minBytes);

    if (minBytes == 0) { return readSoFar; }

    if (branch.buffer.empty()) {
      ZC_IF_SOME(reason, stoppage) {
        // Prefer a short read to an exception. The exception prevents the pull loop from adding any
        // data to the buffer, so `readSoFar` will be zero the next time someone calls `tryRead()`,
        // and the caller will see the exception.
        if (reason.is<Eof>() || readSoFar > 0) { return readSoFar; }
        return cp(reason.get<Exception>());
      }
    }

    auto promise =
        newAdaptedPromise<size_t, ReadSink>(branch.sink, readBuffer, minBytes, readSoFar);
    ensurePulling();
    return mv(promise);
  }

  Maybe<uint64_t> tryGetLength(Branch& branch) {
    return length.map([&branch](uint64_t amount) { return amount + branch.buffer.size(); });
  }

  uint64_t getBufferSizeLimit() const { return bufferSizeLimit; }

  Promise<uint64_t> pumpTo(Branch& branch, AsyncOutputStream& output, uint64_t amount) {
    ZC_ASSERT(branch.sink == zc::none);

    if (amount == 0) { return amount; }

    if (branch.buffer.empty()) {
      ZC_IF_SOME(reason, stoppage) {
        if (reason.is<Eof>()) { return constPromise<uint64_t, 0>(); }
        return cp(reason.get<Exception>());
      }
    }

    auto promise = newAdaptedPromise<uint64_t, PumpSink>(branch.sink, output, amount);
    ensurePulling();
    return mv(promise);
  }

private:
  struct Eof {};
  using Stoppage = OneOf<Eof, Exception>;

  class Sink {
  public:
    struct Need {
      // We use uint64_t here because:
      // - pumpTo() accepts it as the `amount` parameter.
      // - all practical values of tryRead()'s `maxBytes` parameter (a size_t) should also fit into
      //   a uint64_t, unless we're on a machine with multiple exabytes of memory ...

      uint64_t minBytes = 0;

      uint64_t maxBytes = zc::maxValue;
    };

    virtual Promise<void> fill(Buffer& inBuffer, const Maybe<Stoppage>& stoppage) = 0;
    // Attempt to fill the sink with bytes andreturn a promise which must resolve before any inner
    // read may be attempted. If a sink requires backpressure to be respected, this is how it should
    // be communicated.
    //
    // If the sink is full, it must detach from the tee before the returned promise is resolved.
    //
    // The returned promise must not result in an exception.

    virtual Need need() = 0;

    virtual void reject(Exception&& exception) = 0;
    // Inform this sink of a catastrophic exception and detach it. Regular read exceptions should be
    // propagated through `fill()`'s stoppage parameter instead.
  };

  template <typename T>
  class SinkBase : public Sink {
    // Registers itself with the tee as a sink on construction, detaches from the tee on
    // fulfillment, rejection, or destruction.
    //
    // A bit of a Frankenstein, avert your eyes. For one thing, it's more of a mixin than a base...

  public:
    explicit SinkBase(PromiseFulfiller<T>& fulfiller, Maybe<Sink&>& sinkLink)
        : fulfiller(fulfiller), sinkLink(sinkLink) {
      ZC_ASSERT(sinkLink == zc::none, "sink initiated with sink already in flight");
      sinkLink = *this;
    }
    ZC_DISALLOW_COPY_AND_MOVE(SinkBase);
    ~SinkBase() noexcept(false) { detach(); }

    void reject(Exception&& exception) override {
      // The tee is allowed to reject this sink if it needs to, e.g. to propagate a non-inner read
      // exception from the pull loop. Only the derived class is allowed to fulfill() directly,
      // though -- the tee must keep calling fill().

      fulfiller.reject(mv(exception));
      detach();
    }

  protected:
    template <typename U>
    void fulfill(U value) {
      fulfiller.fulfill(fwd<U>(value));
      detach();
    }

  private:
    void detach() {
      ZC_IF_SOME(sink, sinkLink) {
        if (&sink == this) { sinkLink = zc::none; }
      }
    }

    PromiseFulfiller<T>& fulfiller;
    Maybe<Sink&>& sinkLink;
  };

  class ReadSink final : public SinkBase<size_t> {
  public:
    explicit ReadSink(PromiseFulfiller<size_t>& fulfiller, Maybe<Sink&>& registration,
                      ArrayPtr<byte> buffer, size_t minBytes, size_t readSoFar)
        : SinkBase(fulfiller, registration),
          buffer(buffer),
          minBytes(minBytes),
          readSoFar(readSoFar) {}

    Promise<void> fill(Buffer& inBuffer, const Maybe<Stoppage>& stoppage) override {
      auto amount = inBuffer.consume(buffer, minBytes);
      readSoFar += amount;

      if (minBytes == 0) {
        // We satisfied the read request.
        fulfill(readSoFar);
        return READY_NOW;
      }

      if (amount == 0 && inBuffer.empty()) {
        // We made no progress on the read request and the buffer is tapped out.
        ZC_IF_SOME(reason, stoppage) {
          if (reason.is<Eof>() || readSoFar > 0) {
            // Prefer short read to exception.
            fulfill(readSoFar);
          } else {
            reject(cp(reason.get<Exception>()));
          }
          return READY_NOW;
        }
      }

      return READY_NOW;
    }

    Need need() override { return Need{minBytes, buffer.size()}; }

  private:
    ArrayPtr<byte> buffer;
    size_t minBytes;
    // Arguments to the outer tryRead() call, sliced/decremented after every buffer consumption.

    size_t readSoFar;
    // End result of the outer tryRead().
  };

  class PumpSink final : public SinkBase<uint64_t> {
  public:
    explicit PumpSink(PromiseFulfiller<uint64_t>& fulfiller, Maybe<Sink&>& registration,
                      AsyncOutputStream& output, uint64_t limit)
        : SinkBase(fulfiller, registration), output(output), limit(limit) {}

    ~PumpSink() noexcept(false) { canceler.cancel("This pump has been canceled."); }

    Promise<void> fill(Buffer& inBuffer, const Maybe<Stoppage>& stoppage) override {
      ZC_ASSERT(limit > 0);

      uint64_t amount = 0;

      // TODO(someday): This consumes data from the buffer, but we cannot know if the stream to
      //   which we're pumping will accept it until after the write() promise completes. If the
      //   write() promise rejects, we lose this data. We should consume the data from the buffer
      //   only after successful writes.
      auto writeBuffer = inBuffer.asArray(limit, amount);
      ZC_ASSERT(limit >= amount);
      if (amount > 0) {
        Promise<void> promise =
            zc::evalNow([&]() { return output.write(writeBuffer).attach(mv(writeBuffer)); })
                .then([this, amount]() {
                  limit -= amount;
                  pumpedSoFar += amount;
                  if (limit == 0) { fulfill(pumpedSoFar); }
                })
                .eagerlyEvaluate([this](Exception&& exception) { reject(mv(exception)); });

        return canceler.wrap(mv(promise)).catch_([](zc::Exception&&) {});
      } else
        ZC_IF_SOME(reason, stoppage) {
          if (reason.is<Eof>()) {
            // Unlike in the read case, it makes more sense to immediately propagate exceptions to
            // the pump promise rather than show it a "short pump".
            fulfill(pumpedSoFar);
          } else {
            reject(cp(reason.get<Exception>()));
          }
        }

      return READY_NOW;
    }

    Need need() override { return Need{1, limit}; }

  private:
    AsyncOutputStream& output;
    uint64_t limit;
    // Arguments to the outer pumpTo() call, decremented after every buffer consumption.
    //
    // Equal to zero once fulfiller has been fulfilled/rejected.

    uint64_t pumpedSoFar = 0;
    // End result of the outer pumpTo().

    Canceler canceler;
    // When the pump is canceled, we also need to cancel any write operations in flight.
  };

  // =====================================================================================

  Maybe<Sink::Need> analyzeSinks() {
    // Return zc::none if there are no sinks at all. Otherwise, return the largest `minBytes` and
    // the smallest `maxBytes` requested by any sink. The pull loop will use these values to
    // calculate the optimal buffer size for the next inner read, so that a minimum amount of data
    // is buffered at any given time.

    uint64_t minBytes = 0;
    uint64_t maxBytes = zc::maxValue;

    uint nSinks = 0;

    for (auto& branch : branches) {
      ZC_IF_SOME(sink, branch.sink) {
        ++nSinks;
        auto need = sink.need();
        minBytes = zc::max(minBytes, need.minBytes);
        maxBytes = zc::min(maxBytes, need.maxBytes);
      }
    }

    if (nSinks > 0) {
      ZC_ASSERT(minBytes > 0);
      ZC_ASSERT(maxBytes > 0, "sink was filled but did not detach");

      // Sinks may report non-overlapping needs.
      maxBytes = zc::max(minBytes, maxBytes);

      return Sink::Need{minBytes, maxBytes};
    }

    // No active sinks.
    return zc::none;
  }

  void ensurePulling() {
    if (!pulling) {
      pulling = true;
      UnwindDetector unwind;
      ZC_DEFER(if (unwind.isUnwinding()) pulling = false);
      pullPromise = pull();
    }
  }

  Promise<void> pull() {
    return pullLoop().eagerlyEvaluate([this](Exception&& exception) {
      // Exception from our loop, not from inner tryRead(). Something is broken; tell everybody!
      pulling = false;
      for (auto& branch : branches) {
        ZC_IF_SOME(sink, branch.sink) {
          sink.reject(ZC_EXCEPTION(FAILED, "Exception in tee loop", exception));
        }
      }
    });
  }

  constexpr static size_t MAX_BLOCK_SIZE = 1 << 14;  // 16k

  Own<AsyncInputStream> inner;
  const uint64_t bufferSizeLimit = zc::maxValue;
  Maybe<uint64_t> length;
  List<Branch, &Branch::link> branches;
  Maybe<Stoppage> stoppage;
  Promise<void> pullPromise = READY_NOW;
  bool pulling = false;

private:
  Promise<void> pullLoop() {
    // Use evalLater() so that two pump sinks added on the same turn of the event loop will not
    // cause buffering.
    return evalLater([this] {
             // Attempt to fill any sinks that exist.

             Vector<Promise<void>> promises;

             for (auto& branch : branches) {
               ZC_IF_SOME(sink, branch.sink) { promises.add(sink.fill(branch.buffer, stoppage)); }
             }

             // Respect the greatest of the sinks' backpressures.
             return joinPromises(promises.releaseAsArray());
           })
        .then([this]() -> Promise<void> {
          // Check to see whether we need to perform an inner read.

          auto need = analyzeSinks();

          if (need == zc::none) {
            // No more sinks, stop pulling.
            pulling = false;
            return READY_NOW;
          }

          if (stoppage != zc::none) {
            // We're eof or errored, don't read, but loop so we can fill the sink(s).
            return pullLoop();
          }

          auto& n = ZC_ASSERT_NONNULL(need);

          ZC_ASSERT(n.minBytes > 0);

          // We must perform an inner read.

          // We'd prefer not to explode our buffer, if that's cool. We cap `maxBytes` to the buffer
          // size limit or our builtin MAX_BLOCK_SIZE, whichever is smaller. But, we make sure
          // `maxBytes` is still >= `minBytes`.
          n.maxBytes = zc::min(n.maxBytes, MAX_BLOCK_SIZE);
          n.maxBytes = zc::min(n.maxBytes, bufferSizeLimit);
          n.maxBytes = zc::max(n.minBytes, n.maxBytes);
          for (auto& branch : branches) {
            // TODO(perf): buffer.size() is O(n) where n = # of individual heap-allocated byte
            // arrays.
            if (branch.buffer.size() + n.maxBytes > bufferSizeLimit) {
              stoppage = Stoppage(ZC_EXCEPTION(FAILED, "tee buffer size limit exceeded"));
              return pullLoop();
            }
          }
          auto heapBuffer = heapArray<byte>(n.maxBytes);

          // gcc 4.9 quirk: If I don't hoist this into a separate variable and instead call
          //
          //   inner->tryRead(heapBuffer.begin(), n.minBytes, heapBuffer.size())
          //
          // `heapBuffer` seems to get moved into the lambda capture before the arguments to
          // `tryRead()` are evaluated, meaning `inner` sees a nullptr destination. Bizarrely,
          // `inner` sees the correct value for `heapBuffer.size()`... I dunno, man.
          auto destination = heapBuffer.begin();

          return zc::evalNow([&]() { return inner->tryRead(destination, n.minBytes, n.maxBytes); })
              .then(
                  [this, heapBuffer = mv(heapBuffer),
                   minBytes = n.minBytes](size_t amount) mutable -> Promise<void> {
                    length = length.map([amount](uint64_t n) {
                      ZC_ASSERT(n >= amount);
                      return n - amount;
                    });

                    if (amount < heapBuffer.size()) {
                      heapBuffer = heapBuffer.first(amount).attach(mv(heapBuffer));
                    }

                    ZC_ASSERT(stoppage == zc::none);
                    Maybe<ArrayPtr<byte>> bufferPtr = zc::none;
                    for (auto& branch : branches) {
                      // Prefer to move the buffer into the receiving branch's deque, rather than
                      // memcpy.
                      //
                      // TODO(perf): For the 2-branch case, this is fine, since the majority of the
                      // time
                      //   only one buffer will be in use. If we generalize to the n-branch case,
                      //   this would become memcpy-heavy.
                      ZC_IF_SOME(ptr, bufferPtr) { branch.buffer.produce(heapArray(ptr)); }
                      else {
                        bufferPtr = ArrayPtr<byte>(heapBuffer);
                        branch.buffer.produce(mv(heapBuffer));
                      }
                    }

                    if (amount < minBytes) {
                      // Short read, EOF.
                      stoppage = Stoppage(Eof());
                    }

                    return pullLoop();
                  },
                  [this](Exception&& exception) {
                    // Exception from the inner tryRead(). Propagate.
                    stoppage = Stoppage(mv(exception));
                    return pullLoop();
                  });
        });
  }
};

constexpr size_t AsyncTee::MAX_BLOCK_SIZE;

uint64_t AsyncTee::Buffer::consume(ArrayPtr<byte>& readBuffer, size_t& minBytes) {
  uint64_t totalAmount = 0;

  while (readBuffer.size() > 0 && !bufferList.empty()) {
    auto& bytes = bufferList.front();
    auto amount = zc::min(bytes.size(), readBuffer.size());
    memcpy(readBuffer.begin(), bytes.begin(), amount);
    totalAmount += amount;

    readBuffer = readBuffer.slice(amount, readBuffer.size());
    minBytes -= zc::min(amount, minBytes);

    if (amount == bytes.size()) {
      bufferList.pop_front();
    } else {
      bytes = heapArray(bytes.slice(amount, bytes.size()));
      return totalAmount;
    }
  }

  return totalAmount;
}

void AsyncTee::Buffer::produce(Array<byte> bytes) { bufferList.push_back(mv(bytes)); }

Array<const ArrayPtr<const byte>> AsyncTee::Buffer::asArray(uint64_t maxBytes, uint64_t& amount) {
  amount = 0;

  Vector<ArrayPtr<const byte>> buffers;
  Vector<Array<byte>> ownBuffers;

  while (maxBytes > 0 && !bufferList.empty()) {
    auto& bytes = bufferList.front();

    if (bytes.size() <= maxBytes) {
      amount += bytes.size();
      maxBytes -= bytes.size();

      buffers.add(bytes);
      ownBuffers.add(mv(bytes));

      bufferList.pop_front();
    } else {
      auto ownBytes = heapArray(bytes.first(maxBytes));
      buffers.add(ownBytes);
      ownBuffers.add(mv(ownBytes));

      bytes = heapArray(bytes.slice(maxBytes, bytes.size()));

      amount += maxBytes;
      maxBytes = 0;
    }
  }

  if (buffers.size() > 0) { return buffers.releaseAsArray().attach(mv(ownBuffers)); }

  return {};
}

bool AsyncTee::Buffer::empty() const { return bufferList.empty(); }

uint64_t AsyncTee::Buffer::size() const {
  uint64_t result = 0;

  for (auto& bytes : bufferList) { result += bytes.size(); }

  return result;
}

}  // namespace

Tee newTee(Own<AsyncInputStream> input, uint64_t limit) {
  ZC_IF_SOME(t, input->tryTee(limit)) { return {{mv(input), mv(t)}}; }

  auto impl = refcounted<AsyncTee>(mv(input), limit);
  Own<AsyncInputStream> branch1 = heap<AsyncTee::Branch>(addRef(*impl));
  Own<AsyncInputStream> branch2 = heap<AsyncTee::Branch>(mv(impl));
  return {{mv(branch1), mv(branch2)}};
}

namespace {

class PromisedAsyncIoStream final : public zc::AsyncIoStream, private zc::TaskSet::ErrorHandler {
  // An AsyncIoStream which waits for a promise to resolve then forwards all calls to the promised
  // stream.

public:
  PromisedAsyncIoStream(zc::Promise<zc::Own<AsyncIoStream>> promise)
      : promise(promise.then([this](zc::Own<AsyncIoStream> result) { stream = zc::mv(result); })
                    .fork()),
        tasks(*this) {}

  zc::Promise<size_t> read(void* buffer, size_t minBytes, size_t maxBytes) override {
    ZC_IF_SOME(s, stream) { return s->read(buffer, minBytes, maxBytes); }
    else {
      return promise.addBranch().then([this, buffer, minBytes, maxBytes]() {
        return ZC_ASSERT_NONNULL(stream)->read(buffer, minBytes, maxBytes);
      });
    }
  }
  zc::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    ZC_IF_SOME(s, stream) { return s->tryRead(buffer, minBytes, maxBytes); }
    else {
      return promise.addBranch().then([this, buffer, minBytes, maxBytes]() {
        return ZC_ASSERT_NONNULL(stream)->tryRead(buffer, minBytes, maxBytes);
      });
    }
  }

  zc::Maybe<uint64_t> tryGetLength() override {
    ZC_IF_SOME(s, stream) { return s->tryGetLength(); }
    else { return zc::none; }
  }

  zc::Promise<uint64_t> pumpTo(zc::AsyncOutputStream& output, uint64_t amount) override {
    ZC_IF_SOME(s, stream) { return s->pumpTo(output, amount); }
    else {
      return promise.addBranch().then(
          [this, &output, amount]() { return ZC_ASSERT_NONNULL(stream)->pumpTo(output, amount); });
    }
  }

  zc::Promise<void> write(ArrayPtr<const byte> buffer) override {
    ZC_IF_SOME(s, stream) { return s->write(buffer); }
    else {
      return promise.addBranch().then(
          [this, buffer]() { return ZC_ASSERT_NONNULL(stream)->write(buffer); });
    }
  }
  zc::Promise<void> write(zc::ArrayPtr<const zc::ArrayPtr<const byte>> pieces) override {
    ZC_IF_SOME(s, stream) { return s->write(pieces); }
    else {
      return promise.addBranch().then(
          [this, pieces]() { return ZC_ASSERT_NONNULL(stream)->write(pieces); });
    }
  }

  zc::Maybe<zc::Promise<uint64_t>> tryPumpFrom(zc::AsyncInputStream& input,
                                               uint64_t amount = zc::maxValue) override {
    ZC_IF_SOME(s, stream) {
      // Call input.pumpTo() on the resolved stream instead, so that if it does some dynamic_casts
      // or whatnot to detect stream types it can retry those on the inner stream.
      return input.pumpTo(*s, amount);
    }
    else {
      return promise.addBranch().then([this, &input, amount]() {
        // Here we actually have no choice but to call input.pumpTo() because if we called
        // tryPumpFrom(input, amount) and it returned zc::none, what would we do? It's too late for
        // us to return zc::none. But the thing about dynamic_cast also applies.
        return input.pumpTo(*ZC_ASSERT_NONNULL(stream), amount);
      });
    }
  }

  Promise<void> whenWriteDisconnected() override {
    ZC_IF_SOME(s, stream) { return s->whenWriteDisconnected(); }
    else {
      return promise.addBranch().then(
          [this]() { return ZC_ASSERT_NONNULL(stream)->whenWriteDisconnected(); },
          [](zc::Exception&& e) -> zc::Promise<void> {
            if (e.getType() == zc::Exception::Type::DISCONNECTED) {
              return zc::READY_NOW;
            } else {
              return zc::mv(e);
            }
          });
    }
  }

  void shutdownWrite() override {
    ZC_IF_SOME(s, stream) { return s->shutdownWrite(); }
    else {
      tasks.add(promise.addBranch().then(
          [this]() { return ZC_ASSERT_NONNULL(stream)->shutdownWrite(); }));
    }
  }

  void abortRead() override {
    ZC_IF_SOME(s, stream) { return s->abortRead(); }
    else {
      tasks.add(
          promise.addBranch().then([this]() { return ZC_ASSERT_NONNULL(stream)->abortRead(); }));
    }
  }

  zc::Maybe<int> getFd() const override {
    ZC_IF_SOME(s, stream) { return s->getFd(); }
    else { return zc::none; }
  }

private:
  zc::ForkedPromise<void> promise;
  zc::Maybe<zc::Own<AsyncIoStream>> stream;
  zc::TaskSet tasks;

  void taskFailed(zc::Exception&& exception) override { ZC_LOG(ERROR, exception); }
};

class PromisedAsyncOutputStream final : public zc::AsyncOutputStream {
  // An AsyncOutputStream which waits for a promise to resolve then forwards all calls to the
  // promised stream.
  //
  // TODO(cleanup): Can this share implementation with PromiseIoStream? Seems hard.

public:
  PromisedAsyncOutputStream(zc::Promise<zc::Own<AsyncOutputStream>> promise)
      : promise(promise.then([this](zc::Own<AsyncOutputStream> result) { stream = zc::mv(result); })
                    .fork()) {}

  zc::Promise<void> write(ArrayPtr<const byte> buffer) override {
    ZC_IF_SOME(s, stream) { return s->write(buffer); }
    else {
      return promise.addBranch().then(
          [this, buffer]() { return ZC_ASSERT_NONNULL(stream)->write(buffer); });
    }
  }
  zc::Promise<void> write(zc::ArrayPtr<const zc::ArrayPtr<const byte>> pieces) override {
    ZC_IF_SOME(s, stream) { return s->write(pieces); }
    else {
      return promise.addBranch().then(
          [this, pieces]() { return ZC_ASSERT_NONNULL(stream)->write(pieces); });
    }
  }

  zc::Maybe<zc::Promise<uint64_t>> tryPumpFrom(zc::AsyncInputStream& input,
                                               uint64_t amount = zc::maxValue) override {
    ZC_IF_SOME(s, stream) { return s->tryPumpFrom(input, amount); }
    else {
      return promise.addBranch().then([this, &input, amount]() {
        // Call input.pumpTo() on the resolved stream instead.
        return input.pumpTo(*ZC_ASSERT_NONNULL(stream), amount);
      });
    }
  }

  Promise<void> whenWriteDisconnected() override {
    ZC_IF_SOME(s, stream) { return s->whenWriteDisconnected(); }
    else {
      return promise.addBranch().then(
          [this]() { return ZC_ASSERT_NONNULL(stream)->whenWriteDisconnected(); },
          [](zc::Exception&& e) -> zc::Promise<void> {
            if (e.getType() == zc::Exception::Type::DISCONNECTED) {
              return zc::READY_NOW;
            } else {
              return zc::mv(e);
            }
          });
    }
  }

private:
  zc::ForkedPromise<void> promise;
  zc::Maybe<zc::Own<AsyncOutputStream>> stream;
};

}  // namespace

Own<AsyncOutputStream> newPromisedStream(Promise<Own<AsyncOutputStream>> promise) {
  return heap<PromisedAsyncOutputStream>(zc::mv(promise));
}
Own<AsyncIoStream> newPromisedStream(Promise<Own<AsyncIoStream>> promise) {
  return heap<PromisedAsyncIoStream>(zc::mv(promise));
}

Promise<void> AsyncCapabilityStream::writeWithFds(ArrayPtr<const byte> data,
                                                  ArrayPtr<const ArrayPtr<const byte>> moreData,
                                                  ArrayPtr<const AutoCloseFd> fds) {
  // HACK: AutoCloseFd actually contains an `int` under the hood. We can reinterpret_cast to avoid
  //   unnecessary memory allocation.
  static_assert(sizeof(AutoCloseFd) == sizeof(int), "this optimization won't work");
  auto intArray = arrayPtr(reinterpret_cast<const int*>(fds.begin()), fds.size());

  // Be extra-paranoid about aliasing rules by injecting a compiler barrier here. Probably
  // not necessary but also probably doesn't hurt.
#if _MSC_VER
  _ReadWriteBarrier();
#else
  __asm__ __volatile__("" : : : "memory");
#endif

  return writeWithFds(data, moreData, intArray);
}

Promise<Own<AsyncCapabilityStream>> AsyncCapabilityStream::receiveStream() {
  return tryReceiveStream().then(
      [](Maybe<Own<AsyncCapabilityStream>>&& result) -> Promise<Own<AsyncCapabilityStream>> {
        ZC_IF_SOME(r, result) { return zc::mv(r); }
        else { return ZC_EXCEPTION(FAILED, "EOF when expecting to receive capability"); }
      });
}

zc::Promise<Maybe<Own<AsyncCapabilityStream>>> AsyncCapabilityStream::tryReceiveStream() {
  struct ResultHolder {
    byte b;
    Own<AsyncCapabilityStream> stream;
  };
  auto result = zc::heap<ResultHolder>();
  auto promise = tryReadWithStreams(&result->b, 1, 1, &result->stream, 1);
  return promise.then(
      [result = zc::mv(result)](ReadResult actual) mutable -> Maybe<Own<AsyncCapabilityStream>> {
        if (actual.byteCount == 0) { return zc::none; }

        ZC_REQUIRE(
            actual.capCount == 1,
            "expected to receive a capability (e.g. file descriptor via SCM_RIGHTS), but didn't") {
          return zc::none;
        }

        return zc::mv(result->stream);
      });
}

Promise<void> AsyncCapabilityStream::sendStream(Own<AsyncCapabilityStream> stream) {
  static constexpr byte b = 0;
  auto streams = zc::heapArray<Own<AsyncCapabilityStream>>(1);
  streams[0] = zc::mv(stream);
  return writeWithStreams(arrayPtr(&b, 1), nullptr, zc::mv(streams));
}

Promise<AutoCloseFd> AsyncCapabilityStream::receiveFd() {
  return tryReceiveFd().then([](Maybe<AutoCloseFd>&& result) -> Promise<AutoCloseFd> {
    ZC_IF_SOME(r, result) { return zc::mv(r); }
    else { return ZC_EXCEPTION(FAILED, "EOF when expecting to receive capability"); }
  });
}

zc::Promise<zc::Maybe<AutoCloseFd>> AsyncCapabilityStream::tryReceiveFd() {
  struct ResultHolder {
    byte b;
    AutoCloseFd fd;
  };
  auto result = zc::heap<ResultHolder>();
  auto promise = tryReadWithFds(&result->b, 1, 1, &result->fd, 1);
  return promise.then([result = zc::mv(result)](ReadResult actual) mutable -> Maybe<AutoCloseFd> {
    if (actual.byteCount == 0) { return zc::none; }

    ZC_REQUIRE(actual.capCount == 1,
               "expected to receive a file descriptor (e.g. via SCM_RIGHTS), but didn't") {
      return zc::none;
    }

    return zc::mv(result->fd);
  });
}

Promise<void> AsyncCapabilityStream::sendFd(int fd) {
  static constexpr byte b = 0;
  auto fds = zc::heapArray<int>(1);
  fds[0] = fd;
  auto promise = writeWithFds(arrayPtr(&b, 1), nullptr, fds);
  return promise.attach(zc::mv(fds));
}

void AsyncIoStream::getsockopt(int level, int option, void* value, uint* length) {
  ZC_UNIMPLEMENTED("Not a socket.") {
    *length = 0;
    break;
  }
}
void AsyncIoStream::setsockopt(int level, int option, const void* value, uint length) {
  ZC_UNIMPLEMENTED("Not a socket.") { break; }
}
void AsyncIoStream::getsockname(struct sockaddr* addr, uint* length) {
  ZC_UNIMPLEMENTED("Not a socket.") {
    *length = 0;
    break;
  }
}
void AsyncIoStream::getpeername(struct sockaddr* addr, uint* length) {
  ZC_UNIMPLEMENTED("Not a socket.") {
    *length = 0;
    break;
  }
}
void ConnectionReceiver::getsockopt(int level, int option, void* value, uint* length) {
  ZC_UNIMPLEMENTED("Not a socket.") {
    *length = 0;
    break;
  }
}
void ConnectionReceiver::setsockopt(int level, int option, const void* value, uint length) {
  ZC_UNIMPLEMENTED("Not a socket.") { break; }
}
void ConnectionReceiver::getsockname(struct sockaddr* addr, uint* length) {
  ZC_UNIMPLEMENTED("Not a socket.") {
    *length = 0;
    break;
  }
}
void DatagramPort::getsockopt(int level, int option, void* value, uint* length) {
  ZC_UNIMPLEMENTED("Not a socket.") {
    *length = 0;
    break;
  }
}
void DatagramPort::setsockopt(int level, int option, const void* value, uint length) {
  ZC_UNIMPLEMENTED("Not a socket.") { break; }
}
Own<DatagramPort> NetworkAddress::bindDatagramPort() {
  ZC_UNIMPLEMENTED("Datagram sockets not implemented.");
}
Own<DatagramPort> LowLevelAsyncIoProvider::wrapDatagramSocketFd(
    Fd fd, LowLevelAsyncIoProvider::NetworkFilter& filter, uint flags) {
  ZC_UNIMPLEMENTED("Datagram sockets not implemented.");
}
#if !_WIN32
Own<AsyncCapabilityStream> LowLevelAsyncIoProvider::wrapUnixSocketFd(Fd fd, uint flags) {
  ZC_UNIMPLEMENTED("Unix socket with FD passing not implemented.");
}
#endif
CapabilityPipe AsyncIoProvider::newCapabilityPipe() {
  ZC_UNIMPLEMENTED("Capability pipes not implemented.");
}

Own<AsyncInputStream> LowLevelAsyncIoProvider::wrapInputFd(OwnFd&& fd, uint flags) {
  return wrapInputFd(reinterpret_cast<Fd>(fd.release()), flags | TAKE_OWNERSHIP);
}
Own<AsyncOutputStream> LowLevelAsyncIoProvider::wrapOutputFd(OwnFd&& fd, uint flags) {
  return wrapOutputFd(reinterpret_cast<Fd>(fd.release()), flags | TAKE_OWNERSHIP);
}
Own<AsyncIoStream> LowLevelAsyncIoProvider::wrapSocketFd(OwnFd&& fd, uint flags) {
  return wrapSocketFd(reinterpret_cast<Fd>(fd.release()), flags | TAKE_OWNERSHIP);
}
#if !_WIN32
Own<AsyncCapabilityStream> LowLevelAsyncIoProvider::wrapUnixSocketFd(OwnFd&& fd, uint flags) {
  return wrapUnixSocketFd(reinterpret_cast<Fd>(fd.release()), flags | TAKE_OWNERSHIP);
}
#endif
Promise<Own<AsyncIoStream>> LowLevelAsyncIoProvider::wrapConnectingSocketFd(
    OwnFd&& fd, const struct sockaddr* addr, uint addrlen, uint flags) {
  return wrapConnectingSocketFd(reinterpret_cast<Fd>(fd.release()), addr, addrlen,
                                flags | TAKE_OWNERSHIP);
}
Own<ConnectionReceiver> LowLevelAsyncIoProvider::wrapListenSocketFd(OwnFd&& fd,
                                                                    NetworkFilter& filter,
                                                                    uint flags) {
  return wrapListenSocketFd(reinterpret_cast<Fd>(fd.release()), filter, flags | TAKE_OWNERSHIP);
}
Own<ConnectionReceiver> LowLevelAsyncIoProvider::wrapListenSocketFd(OwnFd&& fd, uint flags) {
  return wrapListenSocketFd(reinterpret_cast<Fd>(fd.release()), flags | TAKE_OWNERSHIP);
}
Own<DatagramPort> LowLevelAsyncIoProvider::wrapDatagramSocketFd(OwnFd&& fd, NetworkFilter& filter,
                                                                uint flags) {
  return wrapDatagramSocketFd(reinterpret_cast<Fd>(fd.release()), filter, flags | TAKE_OWNERSHIP);
}
Own<DatagramPort> LowLevelAsyncIoProvider::wrapDatagramSocketFd(OwnFd&& fd, uint flags) {
  return wrapDatagramSocketFd(reinterpret_cast<Fd>(fd.release()), flags | TAKE_OWNERSHIP);
}

namespace {

class DummyNetworkFilter : public zc::LowLevelAsyncIoProvider::NetworkFilter {
public:
  bool shouldAllow(const struct sockaddr* addr, uint addrlen) override { return true; }
};

}  // namespace

LowLevelAsyncIoProvider::NetworkFilter& LowLevelAsyncIoProvider::NetworkFilter::getAllAllowed() {
  static DummyNetworkFilter result;
  return result;
}

// =======================================================================================
// Convenience adapters.

Promise<Own<AsyncIoStream>> CapabilityStreamConnectionReceiver::accept() {
  return inner.receiveStream().then(
      [](Own<AsyncCapabilityStream>&& stream) -> Own<AsyncIoStream> { return zc::mv(stream); });
}

Promise<AuthenticatedStream> CapabilityStreamConnectionReceiver::acceptAuthenticated() {
  return accept().then([](Own<AsyncIoStream>&& stream) {
    return AuthenticatedStream{zc::mv(stream), UnknownPeerIdentity::newInstance()};
  });
}

uint CapabilityStreamConnectionReceiver::getPort() { return 0; }

Promise<Own<AsyncIoStream>> CapabilityStreamNetworkAddress::connect() {
  CapabilityPipe pipe;
  ZC_IF_SOME(p, provider) { pipe = p.newCapabilityPipe(); }
  else { pipe = zc::newCapabilityPipe(); }
  auto result = zc::mv(pipe.ends[0]);
  return inner.sendStream(zc::mv(pipe.ends[1])).then([result = zc::mv(result)]() mutable {
    return Own<AsyncIoStream>(zc::mv(result));
  });
}
Promise<AuthenticatedStream> CapabilityStreamNetworkAddress::connectAuthenticated() {
  return connect().then([](Own<AsyncIoStream>&& stream) {
    return AuthenticatedStream{zc::mv(stream), UnknownPeerIdentity::newInstance()};
  });
}
Own<ConnectionReceiver> CapabilityStreamNetworkAddress::listen() {
  return zc::heap<CapabilityStreamConnectionReceiver>(inner);
}

Own<NetworkAddress> CapabilityStreamNetworkAddress::clone() {
  ZC_UNIMPLEMENTED("can't clone CapabilityStreamNetworkAddress");
}
String CapabilityStreamNetworkAddress::toString() {
  return zc::str("<CapabilityStreamNetworkAddress>");
}

Promise<size_t> FileInputStream::tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
  // Note that our contract with `minBytes` is that we should only return fewer than `minBytes` on
  // EOF. A file read will only produce fewer than the requested number of bytes if EOF was reached.
  // `minBytes` cannot be greater than `maxBytes`. So, this read satisfies the `minBytes`
  // requirement.
  size_t result = file.read(offset, arrayPtr(reinterpret_cast<byte*>(buffer), maxBytes));
  offset += result;
  return result;
}

Maybe<uint64_t> FileInputStream::tryGetLength() {
  uint64_t size = file.stat().size;
  return offset < size ? size - offset : 0;
}

Promise<void> FileOutputStream::write(zc::ArrayPtr<const byte> buffer) {
  file.write(offset, buffer);
  offset += buffer.size();
  return zc::READY_NOW;
}

Promise<void> FileOutputStream::write(ArrayPtr<const ArrayPtr<const byte>> pieces) {
  // TODO(perf): Extend zc::File with an array-of-arrays write?
  for (auto piece : pieces) {
    file.write(offset, piece);
    offset += piece.size();
  }
  return zc::READY_NOW;
}

Promise<void> FileOutputStream::whenWriteDisconnected() { return zc::NEVER_DONE; }

// =======================================================================================

namespace {

class AggregateConnectionReceiver final : public ConnectionReceiver {
public:
  AggregateConnectionReceiver(Array<Own<ConnectionReceiver>> receiversParam)
      : receivers(zc::mv(receiversParam)),
        acceptTasks(zc::heapArray<Maybe<Promise<void>>>(receivers.size())) {}

  Promise<Own<AsyncIoStream>> accept() override {
    return acceptAuthenticated().then(
        [](AuthenticatedStream&& authenticated) { return zc::mv(authenticated.stream); });
  }

  Promise<AuthenticatedStream> acceptAuthenticated() override {
    // Whenever our accept() is called, we want it to resolve to the first connection accepted by
    // any of our child receivers. Naively, it may seem like we should call accept() on them all
    // and exclusiveJoin() the results. Unfortunately, this might not work in a certain race
    // condition: if two or more of our children receive connections simultaneously, both child
    // accept() calls may return, but we'll only end up taking one and dropping the other.
    //
    // To avoid this problem, we must instead initiate `accept()` calls on all children, and even
    // after one of them returns a result, we must allow the others to keep running. If we end up
    // accepting any sockets from children when there is no outstanding accept() on the aggregate,
    // we must put that socket into a backlog. We only restart accept() calls on children if the
    // backlog is empty, and hence the maximum length of the backlog is the number of children
    // minus 1.

    if (backlog.empty()) {
      auto result = zc::newAdaptedPromise<AuthenticatedStream, Waiter>(*this);
      ensureAllAccepting();
      return result;
    } else {
      auto result = zc::mv(backlog.front());
      backlog.pop_front();
      return result;
    }
  }

  uint getPort() override { return receivers.size() > 0 ? receivers[0]->getPort() : 0u; }
  void getsockopt(int level, int option, void* value, uint* length) override {
    ZC_REQUIRE(receivers.size() > 0);
    return receivers[0]->getsockopt(level, option, value, length);
  }
  void setsockopt(int level, int option, const void* value, uint length) override {
    // Apply to all.
    for (auto& r : receivers) { r->setsockopt(level, option, value, length); }
  }
  void getsockname(struct sockaddr* addr, uint* length) override {
    ZC_REQUIRE(receivers.size() > 0);
    return receivers[0]->getsockname(addr, length);
  }

private:
  Array<Own<ConnectionReceiver>> receivers;
  Array<Maybe<Promise<void>>> acceptTasks;

  struct Waiter {
    Waiter(PromiseFulfiller<AuthenticatedStream>& fulfiller, AggregateConnectionReceiver& parent)
        : fulfiller(fulfiller), parent(parent) {
      parent.waiters.add(*this);
    }
    ~Waiter() noexcept(false) {
      if (link.isLinked()) { parent.waiters.remove(*this); }
    }

    PromiseFulfiller<AuthenticatedStream>& fulfiller;
    AggregateConnectionReceiver& parent;
    ListLink<Waiter> link;
  };

  List<Waiter, &Waiter::link> waiters;
  std::deque<Promise<AuthenticatedStream>> backlog;
  // At least one of `waiters` or `backlog` is always empty.

  void ensureAllAccepting() {
    for (auto i : zc::indices(receivers)) {
      if (acceptTasks[i] == zc::none) { acceptTasks[i] = acceptLoop(i); }
    }
  }

  Promise<void> acceptLoop(size_t index) {
    return zc::evalNow([&]() { return receivers[index]->acceptAuthenticated(); })
        .then(
            [this](AuthenticatedStream&& as) {
              if (waiters.empty()) {
                backlog.push_back(zc::mv(as));
              } else {
                auto& waiter = waiters.front();
                waiter.fulfiller.fulfill(zc::mv(as));
                waiters.remove(waiter);
              }
            },
            [this](Exception&& e) {
              if (waiters.empty()) {
                backlog.push_back(zc::mv(e));
              } else {
                auto& waiter = waiters.front();
                waiter.fulfiller.reject(zc::mv(e));
                waiters.remove(waiter);
              }
            })
        .then([this, index]() -> Promise<void> {
          if (waiters.empty()) {
            // Don't keep accepting if there's no one waiting.
            // HACK: We can't cancel ourselves, so detach the task so we can null out the slot.
            //   We know that the promise we're detaching here is exactly the promise that's
            //   currently executing and has no further `.then()`s on it, so no further callbacks
            //   will run in detached state... we're just using `detach()` as a tricky way to have
            //   the event loop dispose of this promise later after we've returned.
            // TODO(cleanup): This pattern has come up several times, we need a better way to handle
            //   it.
            ZC_ASSERT_NONNULL(acceptTasks[index]).detach([](auto&&) {});
            acceptTasks[index] = zc::none;
            return READY_NOW;
          } else {
            return acceptLoop(index);
          }
        });
  }
};

}  // namespace

Own<ConnectionReceiver> newAggregateConnectionReceiver(Array<Own<ConnectionReceiver>> receivers) {
  return zc::heap<AggregateConnectionReceiver>(zc::mv(receivers));
}

// -----------------------------------------------------------------------------

namespace _ {  // private

#if !_WIN32

zc::ArrayPtr<const char> safeUnixPath(const struct sockaddr_un* addr, uint addrlen) {
  ZC_REQUIRE(addr->sun_family == AF_UNIX, "not a unix address");
  ZC_REQUIRE(addrlen >= offsetof(sockaddr_un, sun_path), "invalid unix address");

  size_t maxPathlen = addrlen - offsetof(sockaddr_un, sun_path);

  size_t pathlen;
  if (maxPathlen > 0 && addr->sun_path[0] == '\0') {
    // Linux "abstract" unix address
    pathlen = strnlen(addr->sun_path + 1, maxPathlen - 1) + 1;
  } else {
    pathlen = strnlen(addr->sun_path, maxPathlen);
  }
  return zc::arrayPtr(addr->sun_path, pathlen);
}

#endif  // !_WIN32

ArrayPtr<const CidrRange> localCidrs() {
  static const CidrRange result[] = {
      // localhost
      "127.0.0.0/8"_zc,
      "::1/128"_zc,

      // Trying to *connect* to 0.0.0.0 on many systems is equivalent to connecting to localhost.
      // (wat)
      "0.0.0.0/32"_zc,
      "::/128"_zc,
  };

  // TODO(cleanup): A bug in GCC 4.8, fixed in 4.9, prevents result from implicitly
  //   casting to our return type.
  return zc::arrayPtr(result, zc::size(result));
}

ArrayPtr<const CidrRange> privateCidrs() {
  static const CidrRange result[] = {
      "10.0.0.0/8"_zc,      // RFC1918 reserved for internal network
      "100.64.0.0/10"_zc,   // RFC6598 "shared address space" for carrier-grade NAT
      "169.254.0.0/16"_zc,  // RFC3927 "link local" (auto-configured LAN in absence of DHCP)
      "172.16.0.0/12"_zc,   // RFC1918 reserved for internal network
      "192.168.0.0/16"_zc,  // RFC1918 reserved for internal network

      "fc00::/7"_zc,   // RFC4193 unique private network
      "fe80::/10"_zc,  // RFC4291 "link local" (auto-configured LAN in absence of DHCP)
  };

  // TODO(cleanup): A bug in GCC 4.8, fixed in 4.9, prevents result from implicitly
  //   casting to our return type.
  return zc::arrayPtr(result, zc::size(result));
}

ArrayPtr<const CidrRange> reservedCidrs() {
  // Address ranges reserved by RFCs for specific alternative protocols. These are not considered
  // part of "public", "private", "network", nor "local". But, we will allow apps to explicitly
  // allowlist CIDRs in this range if they really want, because some people actually use these
  // ranges as if they were private ranges.

  static const CidrRange result[] = {
      "192.0.0.0/24"_zc,        // RFC6890 reserved for special protocols
      "224.0.0.0/4"_zc,         // RFC1112 multicast
      "240.0.0.0/4"_zc,         // RFC1112 multicast / reserved for future use
      "255.255.255.255/32"_zc,  // RFC0919 broadcast address

      "2001::/23"_zc,  // RFC2928 reserved for special protocols
      "ff00::/8"_zc,   // RFC4291 multicast
  };

  // TODO(cleanup): A bug in GCC 4.8, fixed in 4.9, prevents result from implicitly
  //   casting to our return type.
  return zc::arrayPtr(result, zc::size(result));
}

ArrayPtr<const CidrRange> exampleAddresses() {
  static const CidrRange result[] = {
      "192.0.2.0/24"_zc,     // RFC5737 "example address" block 1 -- like example.com for IPs
      "198.51.100.0/24"_zc,  // RFC5737 "example address" block 2 -- like example.com for IPs
      "203.0.113.0/24"_zc,   // RFC5737 "example address" block 3 -- like example.com for IPs
      "2001:db8::/32"_zc,    // RFC3849 "example address" block -- like example.com for IPs
  };

  // TODO(cleanup): A bug in GCC 4.8, fixed in 4.9, prevents result from implicitly
  //   casting to our return type.
  return zc::arrayPtr(result, zc::size(result));
}

bool matchesAny(ArrayPtr<const CidrRange> cidrs, const struct sockaddr* addr) {
  for (auto& cidr : cidrs) {
    if (cidr.matches(addr)) return true;
  }
  return false;
}

NetworkFilter::NetworkFilter() : allowUnix(true), allowAbstractUnix(true) {
  allowCidrs.add(CidrRange::inet4({0, 0, 0, 0}, 0));
  allowCidrs.add(CidrRange::inet6({}, {}, 0));
}

NetworkFilter::NetworkFilter(ArrayPtr<const StringPtr> allow, ArrayPtr<const StringPtr> deny,
                             NetworkFilter& next)
    : allowUnix(false), allowAbstractUnix(false), next(next) {
  for (auto rule : allow) {
    if (rule == "local") {
      allowCidrs.addAll(localCidrs());
    } else if (rule == "network") {
      // Can't be represented as a simple union of CIDRs, so we handle in shouldAllow().
      allowNetwork = true;
    } else if (rule == "private") {
      allowCidrs.addAll(privateCidrs());
      allowCidrs.addAll(localCidrs());
    } else if (rule == "public") {
      // Can't be represented as a simple union of CIDRs, so we handle in shouldAllow().
      allowPublic = true;
    } else if (rule == "unix") {
      allowUnix = true;
    } else if (rule == "unix-abstract") {
      allowAbstractUnix = true;
    } else {
      allowCidrs.add(CidrRange(rule));
    }
  }

  for (auto rule : deny) {
    if (rule == "local") {
      denyCidrs.addAll(localCidrs());
    } else if (rule == "network") {
      ZC_FAIL_REQUIRE("don't deny 'network', allow 'local' instead");
    } else if (rule == "private") {
      denyCidrs.addAll(privateCidrs());
    } else if (rule == "public") {
      // Tricky: What if we allow 'network' and deny 'public'?
      ZC_FAIL_REQUIRE("don't deny 'public', allow 'private' instead");
    } else if (rule == "unix") {
      allowUnix = false;
    } else if (rule == "unix-abstract") {
      allowAbstractUnix = false;
    } else {
      denyCidrs.add(CidrRange(rule));
    }
  }
}

bool NetworkFilter::shouldAllow(const struct sockaddr* addr, uint addrlen) {
  ZC_REQUIRE(addrlen >= sizeof(addr->sa_family));

#if !_WIN32
  if (addr->sa_family == AF_UNIX) {
    auto path = safeUnixPath(reinterpret_cast<const struct sockaddr_un*>(addr), addrlen);
    if (path.size() > 0 && path[0] == '\0') {
      return allowAbstractUnix;
    } else {
      return allowUnix;
    }
  }
#endif

  bool allowed = false;
  uint allowSpecificity = 0;

  if (allowPublic) {
    if ((addr->sa_family == AF_INET || addr->sa_family == AF_INET6) &&
        !matchesAny(privateCidrs(), addr) && !matchesAny(localCidrs(), addr) &&
        !matchesAny(reservedCidrs(), addr)) {
      allowed = true;
      // Don't adjust allowSpecificity as this match has an effective specificity of zero.
    }
  }

  if (allowNetwork) {
    if ((addr->sa_family == AF_INET || addr->sa_family == AF_INET6) &&
        !matchesAny(localCidrs(), addr) && !matchesAny(reservedCidrs(), addr)) {
      allowed = true;
      // Don't adjust allowSpecificity as this match has an effective specificity of zero.
    }
  }

  for (auto& cidr : allowCidrs) {
    if (cidr.matches(addr)) {
      allowSpecificity = zc::max(allowSpecificity, cidr.getSpecificity());
      allowed = true;
    }
  }
  if (!allowed) return false;
  for (auto& cidr : denyCidrs) {
    if (cidr.matches(addr)) {
      if (cidr.getSpecificity() >= allowSpecificity) return false;
    }
  }

  ZC_IF_SOME(n, next) { return n.shouldAllow(addr, addrlen); }
  else { return true; }
}

bool NetworkFilter::shouldAllowParse(const struct sockaddr* addr, uint addrlen) {
  bool matched = false;
#if !_WIN32
  if (addr->sa_family == AF_UNIX) {
    auto path = safeUnixPath(reinterpret_cast<const struct sockaddr_un*>(addr), addrlen);
    if (path.size() > 0 && path[0] == '\0') {
      if (allowAbstractUnix) matched = true;
    } else {
      if (allowUnix) matched = true;
    }
  } else {
#endif
    if ((addr->sa_family == AF_INET || addr->sa_family == AF_INET6) &&
        (allowPublic || allowNetwork)) {
      matched = true;
    }
    for (auto& cidr : allowCidrs) {
      if (cidr.matchesFamily(addr->sa_family)) { matched = true; }
    }
#if !_WIN32
  }
#endif

  if (matched) {
    ZC_IF_SOME(n, next) { return n.shouldAllowParse(addr, addrlen); }
    else { return true; }
  } else {
    // No allow rule matches this address family, so don't even allow parsing it.
    return false;
  }
}

}  // namespace _

// =======================================================================================
// PeerIdentity implementations

namespace {

class NetworkPeerIdentityImpl final : public NetworkPeerIdentity {
public:
  NetworkPeerIdentityImpl(zc::Own<NetworkAddress> addr) : addr(zc::mv(addr)) {}

  zc::String toString() override { return addr->toString(); }
  NetworkAddress& getAddress() override { return *addr; }

private:
  zc::Own<NetworkAddress> addr;
};

class LocalPeerIdentityImpl final : public LocalPeerIdentity {
public:
  LocalPeerIdentityImpl(Credentials creds) : creds(creds) {}

  zc::String toString() override {
    char pidBuffer[16]{};
    zc::StringPtr pidStr = nullptr;
    ZC_IF_SOME(p, creds.pid) { pidStr = strPreallocated(pidBuffer, " pid:", p); }

    char uidBuffer[16]{};
    zc::StringPtr uidStr = nullptr;
    ZC_IF_SOME(u, creds.uid) { uidStr = strPreallocated(uidBuffer, " uid:", u); }

    return zc::str("(local peer", pidStr, uidStr, ")");
  }

  Credentials getCredentials() override { return creds; }

private:
  Credentials creds;
};

class UnknownPeerIdentityImpl final : public UnknownPeerIdentity {
public:
  zc::String toString() override { return zc::str("(unknown peer)"); }
};

}  // namespace

zc::Own<NetworkPeerIdentity> NetworkPeerIdentity::newInstance(zc::Own<NetworkAddress> addr) {
  return zc::heap<NetworkPeerIdentityImpl>(zc::mv(addr));
}

zc::Own<LocalPeerIdentity> LocalPeerIdentity::newInstance(LocalPeerIdentity::Credentials creds) {
  return zc::heap<LocalPeerIdentityImpl>(creds);
}

zc::Own<UnknownPeerIdentity> UnknownPeerIdentity::newInstance() {
  static UnknownPeerIdentityImpl instance;
  return {&instance, NullDisposer::instance};
}

Promise<AuthenticatedStream> ConnectionReceiver::acceptAuthenticated() {
  return accept().then([](Own<AsyncIoStream> stream) {
    return AuthenticatedStream{zc::mv(stream), UnknownPeerIdentity::newInstance()};
  });
}

Promise<AuthenticatedStream> NetworkAddress::connectAuthenticated() {
  return connect().then([](Own<AsyncIoStream> stream) {
    return AuthenticatedStream{zc::mv(stream), UnknownPeerIdentity::newInstance()};
  });
}

}  // namespace zc