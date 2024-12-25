// Copyright (c) 2019 Cloudflare, Inc. and contributors
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

#if !_WIN32

#include "src/zc/async/async-unix.h"

#define ZC_XTHREAD_TEST_SETUP_LOOP \
  UnixEventPort port;              \
  EventLoop loop(port);            \
  WaitScope waitScope(loop)

#if _WIN32
#include "src/zc/core/win32-api-version.h"
#endif

#include <src/zc/ztest/test.h>

#include "src/zc/async/async.h"
#include "src/zc/core/debug.h"
#include "src/zc/core/mutex.h"
#include "src/zc/core/thread.h"

#if _WIN32
#include <windows.h>

#include "src/zc/core/windows-sanity.h"
inline void delay() { Sleep(10); }
#else
#include <unistd.h>
inline void delay() { usleep(10000); }
#endif

// This file is #included from async-unix-xthread-test.c++ and async-win32-xthread-test.c++ after
// defining ZC_XTHREAD_TEST_SETUP_LOOP to set up a loop with the corresponding EventPort.
#ifndef ZC_XTHREAD_TEST_SETUP_LOOP
#define ZC_XTHREAD_TEST_SETUP_LOOP \
  EventLoop loop;                  \
  WaitScope waitScope(loop)
#endif

namespace zc {
namespace {

ZC_TEST("synchronous simple cross-thread events") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<uint>> fulfiller;              // accessed only from the subthread
  thread_local bool isChild = false;                  // to assert which thread we're in

  // We use `noexcept` so that any uncaught exceptions immediately terminate the process without
  // unwinding. Otherwise, the unwind would likely deadlock waiting for some synchronization with
  // the other thread.
  Thread thread([&]() noexcept {
    isChild = true;

    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = newPromiseAndFulfiller<uint>();
    fulfiller = zc::mv(paf.fulfiller);

    *executor.lockExclusive() = getCurrentThreadExecutor();

    ZC_ASSERT(paf.promise.wait(waitScope) == 123);

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    const Executor* exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = &ZC_ASSERT_NONNULL(*lock);
    }

    ZC_ASSERT(!isChild);

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE("test exception", exec->executeSync([&]() {
      ZC_ASSERT(isChild);
      ZC_FAIL_ASSERT("test exception") { break; }
    }));

    uint i = exec->executeSync([&]() {
      ZC_ASSERT(isChild);
      fulfiller->fulfill(123);
      return 456;
    });
    ZC_EXPECT(i == 456);

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("asynchronous simple cross-thread events") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<uint>> fulfiller;              // accessed only from the subthread
  thread_local bool isChild = false;                  // to assert which thread we're in

  // We use `noexcept` so that any uncaught exceptions immediately terminate the process without
  // unwinding. Otherwise, the unwind would likely deadlock waiting for some synchronization with
  // the other thread.
  Thread thread([&]() noexcept {
    isChild = true;

    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = newPromiseAndFulfiller<uint>();
    fulfiller = zc::mv(paf.fulfiller);

    *executor.lockExclusive() = getCurrentThreadExecutor();

    ZC_ASSERT(paf.promise.wait(waitScope) == 123);

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    const Executor* exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = &ZC_ASSERT_NONNULL(*lock);
    }

    ZC_ASSERT(!isChild);

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE("test exception", exec->executeAsync([&]() {
                                                                ZC_ASSERT(isChild);
                                                                ZC_FAIL_ASSERT("test exception") {
                                                                  break;
                                                                }
                                                              })
                                                              .wait(waitScope));

    Promise<uint> promise = exec->executeAsync([&]() {
      ZC_ASSERT(isChild);
      fulfiller->fulfill(123);
      return 456u;
    });
    ZC_EXPECT(promise.wait(waitScope) == 456);

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("synchronous promise cross-thread events") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<uint>> fulfiller;              // accessed only from the subthread
  Promise<uint> promise = nullptr;                    // accessed only from the subthread
  thread_local bool isChild = false;                  // to assert which thread we're in

  // We use `noexcept` so that any uncaught exceptions immediately terminate the process without
  // unwinding. Otherwise, the unwind would likely deadlock waiting for some synchronization with
  // the other thread.
  Thread thread([&]() noexcept {
    isChild = true;

    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = newPromiseAndFulfiller<uint>();
    fulfiller = zc::mv(paf.fulfiller);

    auto paf2 = newPromiseAndFulfiller<uint>();
    promise = zc::mv(paf2.promise);

    *executor.lockExclusive() = getCurrentThreadExecutor();

    ZC_ASSERT(paf.promise.wait(waitScope) == 123);

    paf2.fulfiller->fulfill(321);

    // Make sure reply gets sent.
    loop.run();

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    const Executor* exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = &ZC_ASSERT_NONNULL(*lock);
    }

    ZC_ASSERT(!isChild);

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE("test exception", exec->executeSync([&]() {
      ZC_ASSERT(isChild);
      return zc::Promise<void>(ZC_EXCEPTION(FAILED, "test exception"));
    }));

    uint i = exec->executeSync([&]() {
      ZC_ASSERT(isChild);
      fulfiller->fulfill(123);
      return zc::mv(promise);
    });
    ZC_EXPECT(i == 321);

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("asynchronous promise cross-thread events") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<uint>> fulfiller;              // accessed only from the subthread
  Promise<uint> promise = nullptr;                    // accessed only from the subthread
  thread_local bool isChild = false;                  // to assert which thread we're in

  // We use `noexcept` so that any uncaught exceptions immediately terminate the process without
  // unwinding. Otherwise, the unwind would likely deadlock waiting for some synchronization with
  // the other thread.
  Thread thread([&]() noexcept {
    isChild = true;

    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = newPromiseAndFulfiller<uint>();
    fulfiller = zc::mv(paf.fulfiller);

    auto paf2 = newPromiseAndFulfiller<uint>();
    promise = zc::mv(paf2.promise);

    *executor.lockExclusive() = getCurrentThreadExecutor();

    ZC_ASSERT(paf.promise.wait(waitScope) == 123);

    paf2.fulfiller->fulfill(321);

    // Make sure reply gets sent.
    loop.run();

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    const Executor* exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = &ZC_ASSERT_NONNULL(*lock);
    }

    ZC_ASSERT(!isChild);

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE(
        "test exception", exec->executeAsync([&]() {
                                ZC_ASSERT(isChild);
                                return zc::Promise<void>(ZC_EXCEPTION(FAILED, "test exception"));
                              })
                              .wait(waitScope));

    Promise<uint> promise2 = exec->executeAsync([&]() {
      ZC_ASSERT(isChild);
      fulfiller->fulfill(123);
      return zc::mv(promise);
    });
    ZC_EXPECT(promise2.wait(waitScope) == 321);

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("cancel cross-thread event before it runs") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread

  // We use `noexcept` so that any uncaught exceptions immediately terminate the process without
  // unwinding. Otherwise, the unwind would likely deadlock waiting for some synchronization with
  // the other thread.
  Thread thread([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    *executor.lockExclusive() = getCurrentThreadExecutor();

    // We never run the loop here, so that when the event is canceled, it's still queued.

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    const Executor* exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = &ZC_ASSERT_NONNULL(*lock);
    }

    volatile bool called = false;
    {
      Promise<uint> promise = exec->executeAsync([&]() {
        called = true;
        return 123u;
      });
      delay();
      ZC_EXPECT(!promise.poll(waitScope));
    }
    ZC_EXPECT(!called);

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("cancel cross-thread event while it runs") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<void>> fulfiller;              // accessed only from the subthread

  // We use `noexcept` so that any uncaught exceptions immediately terminate the process without
  // unwinding. Otherwise, the unwind would likely deadlock waiting for some synchronization with
  // the other thread.
  Thread thread([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = newPromiseAndFulfiller<void>();
    fulfiller = zc::mv(paf.fulfiller);

    *executor.lockExclusive() = getCurrentThreadExecutor();

    paf.promise.wait(waitScope);

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    const Executor* exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = &ZC_ASSERT_NONNULL(*lock);
    }

    {
      volatile bool called = false;
      Promise<uint> promise = exec->executeAsync([&]() -> zc::Promise<uint> {
        called = true;
        return zc::NEVER_DONE;
      });
      while (!called) { delay(); }
      ZC_EXPECT(!promise.poll(waitScope));
    }

    exec->executeSync([&]() { fulfiller->fulfill(); });

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("cross-thread cancellation in both directions at once") {
  MutexGuarded<zc::Maybe<const Executor&>> childExecutor;
  MutexGuarded<zc::Maybe<const Executor&>> parentExecutor;

  MutexGuarded<uint> readyCount(0);

  thread_local uint threadNumber = 0;
  thread_local bool receivedFinalCall = false;

  // Code to execute simultaneously in two threads...
  // We mark this noexcept so that any exceptions thrown will immediately invoke the termination
  // handler, skipping any destructors that would deadlock.
  auto simultaneous = [&](MutexGuarded<zc::Maybe<const Executor&>>& selfExecutor,
                          MutexGuarded<zc::Maybe<const Executor&>>& otherExecutor,
                          uint threadCount) noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    *selfExecutor.lockExclusive() = getCurrentThreadExecutor();

    const Executor* exec;
    {
      auto lock = otherExecutor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = &ZC_ASSERT_NONNULL(*lock);
    }

    // Create a ton of cross-thread promises to cancel.
    Vector<Promise<void>> promises;
    for (uint i = 0; i < 1000; i++) {
      promises.add(exec->executeAsync([&]() -> zc::Promise<void> {
        return zc::Promise<void>(zc::NEVER_DONE)
            .attach(zc::defer([wasThreadNumber = threadNumber]() {
              // Make sure destruction happens in the correct thread.
              ZC_ASSERT(threadNumber == wasThreadNumber);
            }));
      }));
    }

    // Signal other thread that we're done queueing, and wait for it to signal same.
    {
      auto lock = readyCount.lockExclusive();
      ++*lock;
      lock.wait([&](uint i) { return i >= threadCount; });
    }

    // Run event loop to start all executions queued by the other thread.
    waitScope.poll();
    loop.run();

    // Signal other thread that we've run the loop, and wait for it to signal same.
    {
      auto lock = readyCount.lockExclusive();
      ++*lock;
      lock.wait([&](uint i) { return i >= threadCount * 2; });
    }

    // Cancel all the promises.
    promises.clear();

    // All our cancellations completed, but the other thread may still be waiting for some
    // cancellations from us. We need to pump our event loop to make sure we continue handling
    // those cancellation requests. In particular we'll queue a function to the other thread and
    // wait for it to complete. The other thread will queue its own function to this thread just
    // before completing the function we queued to it.
    receivedFinalCall = false;
    exec->executeAsync([&]() { receivedFinalCall = true; }).wait(waitScope);

    // To be safe, make sure we've actually executed the function that the other thread queued to
    // us by repeatedly polling until `receivedFinalCall` becomes true in this thread.
    while (!receivedFinalCall) {
      waitScope.poll();
      loop.run();
    }

    // OK, signal other that we're all done.
    *otherExecutor.lockExclusive() = zc::none;

    // Wait until other thread sets executor to null, as a way to tell us to quit.
    selfExecutor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  };

  {
    Thread thread([&]() {
      threadNumber = 1;
      simultaneous(childExecutor, parentExecutor, 2);
    });

    threadNumber = 0;
    simultaneous(parentExecutor, childExecutor, 2);
  }

  // Let's even have a three-thread version, with cyclic cancellation requests.
  MutexGuarded<zc::Maybe<const Executor&>> child2Executor;
  *readyCount.lockExclusive() = 0;

  {
    Thread thread1([&]() {
      threadNumber = 1;
      simultaneous(childExecutor, child2Executor, 3);
    });

    Thread thread2([&]() {
      threadNumber = 2;
      simultaneous(child2Executor, parentExecutor, 3);
    });

    threadNumber = 0;
    simultaneous(parentExecutor, childExecutor, 3);
  }
}

ZC_TEST("cross-thread cancellation cycle") {
  // Another multi-way cancellation test where we set up an actual cycle between three threads
  // waiting on each other to complete a single event.

  MutexGuarded<zc::Maybe<const Executor&>> child1Executor, child2Executor;

  Own<PromiseFulfiller<void>> fulfiller1, fulfiller2;

  auto threadMain = [](MutexGuarded<zc::Maybe<const Executor&>>& executor,
                       Own<PromiseFulfiller<void>>& fulfiller) noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = newPromiseAndFulfiller<void>();
    fulfiller = zc::mv(paf.fulfiller);

    *executor.lockExclusive() = getCurrentThreadExecutor();

    paf.promise.wait(waitScope);

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  };

  Thread thread1([&]() noexcept { threadMain(child1Executor, fulfiller1); });
  Thread thread2([&]() noexcept { threadMain(child2Executor, fulfiller2); });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;
    auto& parentExecutor = getCurrentThreadExecutor();

    const Executor* exec1;
    {
      auto lock = child1Executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec1 = &ZC_ASSERT_NONNULL(*lock);
    }
    const Executor* exec2;
    {
      auto lock = child2Executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec2 = &ZC_ASSERT_NONNULL(*lock);
    }

    // Create an event that cycles through both threads and back to this one, and then cancel it.
    bool cycleAllDestroyed = false;
    {
      auto paf = zc::newPromiseAndFulfiller<void>();
      Promise<uint> promise = exec1->executeAsync([&]() -> zc::Promise<uint> {
        return exec2->executeAsync([&]() -> zc::Promise<uint> {
          return parentExecutor.executeAsync([&]() -> zc::Promise<uint> {
            paf.fulfiller->fulfill();
            return zc::Promise<uint>(zc::NEVER_DONE).attach(zc::defer([&]() {
              cycleAllDestroyed = true;
            }));
          });
        });
      });

      // Wait until the cycle has come all the way around.
      paf.promise.wait(waitScope);

      ZC_EXPECT(!promise.poll(waitScope));
    }

    ZC_EXPECT(cycleAllDestroyed);

    exec1->executeSync([&]() { fulfiller1->fulfill(); });
    exec2->executeSync([&]() { fulfiller2->fulfill(); });

    *child1Executor.lockExclusive() = zc::none;
    *child2Executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("call own thread's executor") {
  ZC_XTHREAD_TEST_SETUP_LOOP;

  auto& executor = getCurrentThreadExecutor();

  {
    uint i = executor.executeSync([]() { return 123u; });
    ZC_EXPECT(i == 123);
  }

  ZC_EXPECT_THROW_MESSAGE(
      "can't call executeSync() on own thread's executor with a promise-returning function",
      executor.executeSync([]() { return zc::yield(); }));

  {
    uint i = executor.executeAsync([]() { return 123u; }).wait(waitScope);
    ZC_EXPECT(i == 123);
  }
}

ZC_TEST("synchronous cross-thread event disconnected") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<void>> fulfiller;              // accessed only from the subthread
  thread_local bool isChild = false;                  // to assert which thread we're in

  Thread thread([&]() noexcept {
    isChild = true;

    {
      ZC_XTHREAD_TEST_SETUP_LOOP;

      auto paf = newPromiseAndFulfiller<void>();
      fulfiller = zc::mv(paf.fulfiller);

      *executor.lockExclusive() = getCurrentThreadExecutor();

      paf.promise.wait(waitScope);

      // Exit the event loop!
    }

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    Own<const Executor> exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = ZC_ASSERT_NONNULL(*lock).addRef();
    }

    ZC_EXPECT(!isChild);

    ZC_EXPECT(exec->isLive());

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE(
        "Executor's event loop exited before cross-thread event could complete",
        exec->executeSync([&]() -> Promise<void> {
          fulfiller->fulfill();
          return zc::NEVER_DONE;
        }));

    ZC_EXPECT(!exec->isLive());

    ZC_EXPECT_THROW_MESSAGE("Executor's event loop has exited", exec->executeSync([&]() {}));

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("asynchronous cross-thread event disconnected") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<void>> fulfiller;              // accessed only from the subthread
  thread_local bool isChild = false;                  // to assert which thread we're in

  Thread thread([&]() noexcept {
    isChild = true;

    {
      ZC_XTHREAD_TEST_SETUP_LOOP;

      auto paf = newPromiseAndFulfiller<void>();
      fulfiller = zc::mv(paf.fulfiller);

      *executor.lockExclusive() = getCurrentThreadExecutor();

      paf.promise.wait(waitScope);

      // Exit the event loop!
    }

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    Own<const Executor> exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = ZC_ASSERT_NONNULL(*lock).addRef();
    }

    ZC_EXPECT(!isChild);

    ZC_EXPECT(exec->isLive());

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE(
        "Executor's event loop exited before cross-thread event could complete",
        exec->executeAsync([&]() -> Promise<void> {
              fulfiller->fulfill();
              return zc::NEVER_DONE;
            })
            .wait(waitScope));

    ZC_EXPECT(!exec->isLive());

    ZC_EXPECT_THROW_MESSAGE("Executor's event loop has exited",
                            exec->executeAsync([&]() {}).wait(waitScope));

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("cross-thread event disconnected before it runs") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  thread_local bool isChild = false;                  // to assert which thread we're in

  Thread thread([&]() noexcept {
    isChild = true;

    ZC_XTHREAD_TEST_SETUP_LOOP;

    *executor.lockExclusive() = getCurrentThreadExecutor();

    // Don't actually run the event loop. Destroy it when the other thread signals us to.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    Own<const Executor> exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = ZC_ASSERT_NONNULL(*lock).addRef();
    }

    ZC_EXPECT(!isChild);

    ZC_EXPECT(exec->isLive());

    auto promise = exec->executeAsync([&]() { ZC_LOG(ERROR, "shouldn't have executed"); });
    ZC_EXPECT(!promise.poll(waitScope));

    *executor.lockExclusive() = zc::none;

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE(
        "Executor's event loop exited before cross-thread event could complete",
        promise.wait(waitScope));

    ZC_EXPECT(!exec->isLive());
  })();
}

ZC_TEST("cross-thread event disconnected without holding Executor ref") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<void>> fulfiller;              // accessed only from the subthread
  thread_local bool isChild = false;                  // to assert which thread we're in

  Thread thread([&]() noexcept {
    isChild = true;

    {
      ZC_XTHREAD_TEST_SETUP_LOOP;

      auto paf = newPromiseAndFulfiller<void>();
      fulfiller = zc::mv(paf.fulfiller);

      *executor.lockExclusive() = getCurrentThreadExecutor();

      paf.promise.wait(waitScope);

      // Exit the event loop!
    }

    // Wait until parent thread sets executor to null, as a way to tell us to quit.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    const Executor* exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = &ZC_ASSERT_NONNULL(*lock);
    }

    ZC_EXPECT(!isChild);

    ZC_EXPECT(exec->isLive());

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE(
        "Executor's event loop exited before cross-thread event could complete",
        exec->executeSync([&]() -> Promise<void> {
          fulfiller->fulfill();
          return zc::NEVER_DONE;
        }));

    // Can't check `exec->isLive()` because it's been destroyed by now.

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("detached cross-thread event doesn't cause crash") {
  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<void>> fulfiller;              // accessed only from the subthread

  Thread thread([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = newPromiseAndFulfiller<void>();
    fulfiller = zc::mv(paf.fulfiller);

    *executor.lockExclusive() = getCurrentThreadExecutor();

    paf.promise.wait(waitScope);

    // Without this poll(), we don't attempt to reply to the other thread? But this isn't required
    // in other tests, for some reason? Oh well.
    waitScope.poll();

    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });
  });

  ([&]() noexcept {
    {
      ZC_XTHREAD_TEST_SETUP_LOOP;

      const Executor* exec;
      {
        auto lock = executor.lockExclusive();
        lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
        exec = &ZC_ASSERT_NONNULL(*lock);
      }

      exec->executeAsync([&]() -> zc::Promise<void> {
            // Make sure other thread gets time to exit its EventLoop.
            delay();
            delay();
            delay();
            fulfiller->fulfill();
            return zc::READY_NOW;
          })
          .detach([&](zc::Exception&& e) { ZC_LOG(ERROR, e); });

      // Give the other thread a chance to wake up and start working on the event.
      delay();

      // Now we'll destroy our EventLoop. That *should* cause detached promises to be destroyed,
      // thereby cancelling it, before disabling our own executor. However, at one point in the
      // past, our executor was shut down first, followed by destroying detached promises, which
      // led to an abort because the other thread had no way to reply back to this thread.
    }

    *executor.lockExclusive() = zc::none;
  })();
}

ZC_TEST("cross-thread event cancel requested while destination thread being destroyed") {
  // This exercises the code in Executor::Impl::disconnect() which tears down the list of
  // cross-thread events which have already been canceled. At one point this code had a bug which
  // would cause it to throw if any events were present in the cancel list.

  MutexGuarded<zc::Maybe<const Executor&>> executor;  // to get the Executor from the other thread
  Own<PromiseFulfiller<void>> fulfiller;              // accessed only from the subthread

  Thread thread([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = newPromiseAndFulfiller<void>();
    fulfiller = zc::mv(paf.fulfiller);

    *executor.lockExclusive() = getCurrentThreadExecutor();

    // Wait for other thread to start a cross-thread task.
    paf.promise.wait(waitScope);

    // Let the other thread know, out-of-band, that the task is running, so that it can now request
    // cancellation. We do this by setting `executor` to null (but we could also use some separate
    // MutexGuarded conditional variable instead).
    *executor.lockExclusive() = zc::none;

    // Give other thread a chance to request cancellation of the promise.
    delay();

    // now we exit the event loop
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    const Executor* exec;
    {
      auto lock = executor.lockExclusive();
      lock.wait([&](zc::Maybe<const Executor&> value) { return value != zc::none; });
      exec = &ZC_ASSERT_NONNULL(*lock);
    }

    ZC_EXPECT(exec->isLive());

    auto promise = exec->executeAsync([&]() -> Promise<void> {
      fulfiller->fulfill();
      return zc::NEVER_DONE;
    });

    // Wait for the other thread to signal to us that it has indeed started executing our task.
    executor.lockExclusive().wait([](auto& val) { return val == zc::none; });

    // Cancel the promise.
    promise = nullptr;
  })();
}

ZC_TEST("cross-thread fulfiller") {
  MutexGuarded<Maybe<Own<PromiseFulfiller<int>>>> fulfillerMutex;

  Thread thread([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = zc::newPromiseAndCrossThreadFulfiller<int>();
    *fulfillerMutex.lockExclusive() = zc::mv(paf.fulfiller);

    int result = paf.promise.wait(waitScope);
    ZC_EXPECT(result == 123);
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    Own<PromiseFulfiller<int>> fulfiller;
    {
      auto lock = fulfillerMutex.lockExclusive();
      lock.wait([&](auto& value) { return value != zc::none; });
      fulfiller = zc::mv(ZC_ASSERT_NONNULL(*lock));
    }

    fulfiller->fulfill(123);
  })();
}

ZC_TEST("cross-thread fulfiller rejects") {
  MutexGuarded<Maybe<Own<PromiseFulfiller<void>>>> fulfillerMutex;

  Thread thread([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = zc::newPromiseAndCrossThreadFulfiller<void>();
    *fulfillerMutex.lockExclusive() = zc::mv(paf.fulfiller);

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE("foo exception", paf.promise.wait(waitScope));
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    Own<PromiseFulfiller<void>> fulfiller;
    {
      auto lock = fulfillerMutex.lockExclusive();
      lock.wait([&](auto& value) { return value != zc::none; });
      fulfiller = zc::mv(ZC_ASSERT_NONNULL(*lock));
    }

    fulfiller->reject(ZC_EXCEPTION(FAILED, "foo exception"));
  })();
}

ZC_TEST("cross-thread fulfiller destroyed") {
  MutexGuarded<Maybe<Own<PromiseFulfiller<void>>>> fulfillerMutex;

  Thread thread([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = zc::newPromiseAndCrossThreadFulfiller<void>();
    *fulfillerMutex.lockExclusive() = zc::mv(paf.fulfiller);

    ZC_EXPECT_THROW_RECOVERABLE_MESSAGE(
        "cross-thread PromiseFulfiller was destroyed without fulfilling the promise",
        paf.promise.wait(waitScope));
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    Own<PromiseFulfiller<void>> fulfiller;
    {
      auto lock = fulfillerMutex.lockExclusive();
      lock.wait([&](auto& value) { return value != zc::none; });
      fulfiller = zc::mv(ZC_ASSERT_NONNULL(*lock));
    }

    fulfiller = nullptr;
  })();
}

ZC_TEST("cross-thread fulfiller canceled") {
  MutexGuarded<Maybe<Own<PromiseFulfiller<void>>>> fulfillerMutex;
  MutexGuarded<bool> done;

  Thread thread([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = zc::newPromiseAndCrossThreadFulfiller<void>();
    {
      auto lock = fulfillerMutex.lockExclusive();
      *lock = zc::mv(paf.fulfiller);
      lock.wait([](auto& value) { return value == zc::none; });
    }

    // cancel
    paf.promise = nullptr;

    {
      auto lock = done.lockExclusive();
      lock.wait([](bool value) { return value; });
    }
  });

  ([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    Own<PromiseFulfiller<void>> fulfiller;
    {
      auto lock = fulfillerMutex.lockExclusive();
      lock.wait([&](auto& value) { return value != zc::none; });
      fulfiller = zc::mv(ZC_ASSERT_NONNULL(*lock));
      ZC_ASSERT(fulfiller->isWaiting());
      *lock = zc::none;
    }

    // Should eventually show not waiting.
    while (fulfiller->isWaiting()) { delay(); }

    *done.lockExclusive() = true;
  })();
}

ZC_TEST("cross-thread fulfiller multiple fulfills") {
  MutexGuarded<Maybe<Own<PromiseFulfiller<int>>>> fulfillerMutex;

  Thread thread([&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    auto paf = zc::newPromiseAndCrossThreadFulfiller<int>();
    *fulfillerMutex.lockExclusive() = zc::mv(paf.fulfiller);

    int result = paf.promise.wait(waitScope);
    ZC_EXPECT(result == 123);
  });

  auto func = [&]() noexcept {
    ZC_XTHREAD_TEST_SETUP_LOOP;

    PromiseFulfiller<int>* fulfiller;
    {
      auto lock = fulfillerMutex.lockExclusive();
      lock.wait([&](auto& value) { return value != zc::none; });
      fulfiller = ZC_ASSERT_NONNULL(*lock).get();
    }

    fulfiller->fulfill(123);
  };

  zc::Thread thread1(func);
  zc::Thread thread2(func);
  zc::Thread thread3(func);
  zc::Thread thread4(func);
}

}  // namespace
}  // namespace zc

#endif  // !_WIN32