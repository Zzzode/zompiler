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

#if _WIN32 || __CYGWIN__
#include "zc/core/win32-api-version.h"
#endif

#include "zc/core/debug.h"
#include "zc/core/mutex.h"

#if !_WIN32 && !__CYGWIN__
#include <errno.h>
#include <time.h>
#endif

#if ZC_USE_FUTEX
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_futex
// Missing on Android/Bionic.
#ifdef __NR_futex
#define SYS_futex __NR_futex
#elif defined(SYS_futex_time64)
#define SYS_futex SYS_futex_time64
#else
#error "Need working SYS_futex"
#endif
#endif

#ifndef FUTEX_WAIT_PRIVATE
// Missing on Android/Bionic.
#define FUTEX_WAIT_PRIVATE FUTEX_WAIT
#define FUTEX_WAKE_PRIVATE FUTEX_WAKE
#endif

#elif _WIN32 || __CYGWIN__
#include <windows.h>
#endif

namespace zc {
#ifdef ZC_USE_FUTEX
struct BlockedOnMutexAcquisition {
  constexpr BlockedOnMutexAcquisition(const _::Mutex& mutex, LockSourceLocationArg) {}
};

struct BlockedOnCondVarWait {
  constexpr BlockedOnCondVarWait(const _::Mutex& mutex, const void* waiter, LockSourceLocationArg) {
  }
};

struct BlockedOnOnceInit {
  constexpr BlockedOnOnceInit(const _::Once& once, LockSourceLocationArg) {}
};

struct BlockedOnReason {
  constexpr BlockedOnReason(const BlockedOnMutexAcquisition&) {}
  constexpr BlockedOnReason(const BlockedOnCondVarWait&) {}
  constexpr BlockedOnReason(const BlockedOnOnceInit&) {}
};

static void setCurrentThreadIsWaitingFor(const BlockedOnReason* meta) {}
static void setCurrentThreadIsNoLongerWaiting() {}
#endif

namespace _ {  // private

#if ZC_USE_FUTEX
constexpr uint Mutex::EXCLUSIVE_HELD;
constexpr uint Mutex::EXCLUSIVE_REQUESTED;
constexpr uint Mutex::SHARED_COUNT_MASK;
#endif

inline void Mutex::addWaiter(Waiter& waiter) {
#ifdef ZC_DEBUG
  assertLockedByCaller(EXCLUSIVE);
#endif
  *waitersTail = waiter;
  waitersTail = &waiter.next;
}
inline void Mutex::removeWaiter(Waiter& waiter) {
#ifdef ZC_DEBUG
  assertLockedByCaller(EXCLUSIVE);
#endif
  *waiter.prev = waiter.next;
  ZC_IF_SOME(next, waiter.next) { next.prev = waiter.prev; }
  else {
    ZC_DASSERT(waitersTail == &waiter.next);
    waitersTail = waiter.prev;
  }
}

bool Mutex::checkPredicate(Waiter& waiter) {
  // Run the predicate from a thread other than the waiting thread, returning true if it's time to
  // signal the waiting thread. This is not only when the predicate passes, but also when it
  // throws, in which case we want to propagate the exception to the waiting thread.

  if (waiter.exception != zc::none) return true;  // don't run again after an exception

  bool result = false;
  ZC_IF_SOME(exception, zc::runCatchingExceptions([&]() { result = waiter.predicate.check(); })) {
    // Exception thrown.
    result = true;
    waiter.exception = zc::heap(zc::mv(exception));
  };
  return result;
}

#if !_WIN32 && !__CYGWIN__
namespace {

TimePoint toTimePoint(struct timespec ts) {
  return zc::origin<TimePoint>() + ts.tv_sec * zc::SECONDS + ts.tv_nsec * zc::NANOSECONDS;
}
TimePoint now() {
  struct timespec now;
  ZC_SYSCALL(clock_gettime(CLOCK_MONOTONIC, &now));
  return toTimePoint(now);
}
struct timespec toRelativeTimespec(Duration timeout) {
  struct timespec ts;
  ts.tv_sec = timeout / zc::SECONDS;
  ts.tv_nsec = timeout % zc::SECONDS / zc::NANOSECONDS;
  return ts;
}
struct timespec toAbsoluteTimespec(TimePoint time) {
  return toRelativeTimespec(time - zc::origin<TimePoint>());
}

}  // namespace
#endif

#if ZC_USE_FUTEX
// =======================================================================================
// Futex-based implementation (Linux-only)

Mutex::Mutex() : futex(0) {}
Mutex::~Mutex() {
  // This will crash anyway, might as well crash with a nice error message.
  ZC_ASSERT(futex == 0, "Mutex destroyed while locked.") { break; }
}

bool Mutex::lock(Exclusivity exclusivity, Maybe<Duration> timeout, LockSourceLocationArg location) {
  BlockedOnReason blockReason = BlockedOnMutexAcquisition{*this, location};
  ZC_DEFER(setCurrentThreadIsNoLongerWaiting());

  auto spec = timeout.map([](Duration d) { return toRelativeTimespec(d); });
  struct timespec* specp = nullptr;
  ZC_IF_SOME(s, spec) { specp = &s; }

  switch (exclusivity) {
    case EXCLUSIVE:
      for (;;) {
        uint state = 0;
        if (ZC_LIKELY(__atomic_compare_exchange_n(&futex, &state, EXCLUSIVE_HELD, false,
                                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))) {
          // Acquired.
          break;
        }

        // The mutex is contended.  Set the exclusive-requested bit and wait.
        if ((state & EXCLUSIVE_REQUESTED) == 0) {
          if (!__atomic_compare_exchange_n(&futex, &state, state | EXCLUSIVE_REQUESTED, false,
                                           __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            // Oops, the state changed before we could set the request bit.  Start over.
            continue;
          }

          state |= EXCLUSIVE_REQUESTED;
        }

        setCurrentThreadIsWaitingFor(&blockReason);

        auto result = syscall(SYS_futex, &futex, FUTEX_WAIT_PRIVATE, state, specp, nullptr, 0);
        if (result < 0) {
          if (errno == ETIMEDOUT) {
            setCurrentThreadIsNoLongerWaiting();
            // We timed out, we can't remove the exclusive request flag (since others might be
            // waiting) so we just return false.
            return false;
          }
        }
      }
#if ZC_CONTENTION_WARNING_THRESHOLD
      printContendedReader = false;
#endif
      break;
    case SHARED: {
#if ZC_CONTENTION_WARNING_THRESHOLD
      zc::Maybe<zc::TimePoint> contentionWaitStart;
#endif

      uint state = __atomic_add_fetch(&futex, 1, __ATOMIC_ACQUIRE);

      for (;;) {
        if (ZC_LIKELY((state & EXCLUSIVE_HELD) == 0)) {
          // Acquired.
          break;
        }

#if ZC_CONTENTION_WARNING_THRESHOLD
        if (contentionWaitStart == zc::none) {
          // We could have the exclusive mutex tell us how long it was holding the lock. That would
          // be the nicest. However, I'm hesitant to bloat the structure. I suspect having a reader
          // tell us how long it was waiting for is probably a good proxy.
          contentionWaitStart = zc::systemPreciseMonotonicClock().now();
        }
#endif

        setCurrentThreadIsWaitingFor(&blockReason);

        // The mutex is exclusively locked by another thread.  Since we incremented the counter
        // already, we just have to wait for it to be unlocked.
        auto result = syscall(SYS_futex, &futex, FUTEX_WAIT_PRIVATE, state, specp, nullptr, 0);
        if (result < 0) {
          // If we timeout though, we need to signal that we're not waiting anymore.
          if (errno == ETIMEDOUT) {
            setCurrentThreadIsNoLongerWaiting();
            state = __atomic_sub_fetch(&futex, 1, __ATOMIC_RELAXED);

            // We may have unlocked since we timed out. So act like we just unlocked the mutex
            // and maybe send a wait signal if needed. See Mutex::unlock SHARED case.
            if (ZC_UNLIKELY(state == EXCLUSIVE_REQUESTED)) {
              if (__atomic_compare_exchange_n(&futex, &state, 0, false, __ATOMIC_RELAXED,
                                              __ATOMIC_RELAXED)) {
                // Wake all exclusive waiters.  We have to wake all of them because one of them will
                // grab the lock while the others will re-establish the exclusive-requested bit.
                syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
              }
            }
            return false;
          }
        }
        state = __atomic_load_n(&futex, __ATOMIC_ACQUIRE);
      }

#ifdef ZC_CONTENTION_WARNING_THRESHOLD
      ZC_IF_SOME(start, contentionWaitStart) {
        if (__atomic_load_n(&printContendedReader, __ATOMIC_RELAXED)) {
          // Double-checked lock avoids the CPU needing to acquire the lock in most cases.
          if (__atomic_exchange_n(&printContendedReader, false, __ATOMIC_RELAXED)) {
            auto contentionDuration = zc::systemPreciseMonotonicClock().now() - start;
            ZC_LOG(WARNING, "Acquired contended lock", location, contentionDuration,
                   zc::getStackTrace());
          }
        }
      }
#endif

      break;
    }
  }
  return true;
}

void Mutex::unlock(Exclusivity exclusivity, Waiter* waiterToSkip) {
  switch (exclusivity) {
    case EXCLUSIVE: {
      ZC_DASSERT(futex & EXCLUSIVE_HELD, "Unlocked a mutex that wasn't locked.");

#ifdef ZC_CONTENTION_WARNING_THRESHOLD
      auto acquiredLocation = releasingExclusive();
#endif

      // First check if there are any conditional waiters. Note we only do this when unlocking an
      // exclusive lock since under a shared lock the state couldn't have changed.
      auto nextWaiter = waitersHead;
      for (;;) {
        ZC_IF_SOME(waiter, nextWaiter) {
          nextWaiter = waiter.next;

          if (&waiter != waiterToSkip && checkPredicate(waiter)) {
            // This waiter's predicate now evaluates true, so wake it up.
            if (waiter.hasTimeout) {
              // In this case we need to be careful to make sure the target thread isn't already
              // processing a timeout, so we need to do an atomic CAS rather than just a store.
              uint expected = 0;
              if (__atomic_compare_exchange_n(&waiter.futex, &expected, 1, false, __ATOMIC_RELEASE,
                                              __ATOMIC_RELAXED)) {
                // Good, we set it to 1, transferring ownership of the mutex. Continue on below.
              } else {
                // Looks like the thread already timed out and set its own futex to 1. In that
                // case it is going to try to lock the mutex itself, so we should NOT attempt an
                // ownership transfer as this will deadlock.
                //
                // We have two options here: We can continue along the waiter list looking for
                // another waiter that's ready to be signaled, or we could drop out of the list
                // immediately since we know that another thread is already waiting for the lock
                // and will re-evaluate the waiter queue itself when it is done. It feels cleaner
                // to me to continue.
                continue;
              }
            } else {
              __atomic_store_n(&waiter.futex, 1, __ATOMIC_RELEASE);
            }
            syscall(SYS_futex, &waiter.futex, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);

            // We transferred ownership of the lock to this waiter, so we're done now.
            return;
          }
        }
        else {
          // No more waiters.
          break;
        }
      }

#ifdef ZC_CONTENTION_WARNING_THRESHOLD
      uint readerCount;
      {
        uint oldState = __atomic_load_n(&futex, __ATOMIC_RELAXED);
        readerCount = oldState & SHARED_COUNT_MASK;
        if (readerCount >= ZC_CONTENTION_WARNING_THRESHOLD) {
          // Atomic not needed because we're still holding the exclusive lock.
          printContendedReader = true;
        }
      }
#endif

      // Didn't wake any waiters, so wake normally.
      uint oldState =
          __atomic_fetch_and(&futex, ~(EXCLUSIVE_HELD | EXCLUSIVE_REQUESTED), __ATOMIC_RELEASE);

      if (ZC_UNLIKELY(oldState & ~EXCLUSIVE_HELD)) {
        // Other threads are waiting.  If there are any shared waiters, they now collectively hold
        // the lock, and we must wake them up.  If there are any exclusive waiters, we must wake
        // them up even if readers are waiting so that at the very least they may re-establish the
        // EXCLUSIVE_REQUESTED bit that we just removed.
        syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);

#ifdef ZC_CONTENTION_WARNING_THRESHOLD
        if (readerCount >= ZC_CONTENTION_WARNING_THRESHOLD) {
          ZC_LOG(WARNING, "excessively many readers were waiting on this lock", readerCount,
                 acquiredLocation, zc::getStackTrace());
        }
#endif
      }
      break;
    }

    case SHARED: {
      ZC_DASSERT(futex & SHARED_COUNT_MASK, "Unshared a mutex that wasn't shared.");
      uint state = __atomic_sub_fetch(&futex, 1, __ATOMIC_RELEASE);

      // The only case where anyone is waiting is if EXCLUSIVE_REQUESTED is set, and the only time
      // it makes sense to wake up that waiter is if the shared count has reached zero.
      if (ZC_UNLIKELY(state == EXCLUSIVE_REQUESTED)) {
        if (__atomic_compare_exchange_n(&futex, &state, 0, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
          // Wake all exclusive waiters.  We have to wake all of them because one of them will
          // grab the lock while the others will re-establish the exclusive-requested bit.
          syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
        }
      }
      break;
    }
  }
}

void Mutex::assertLockedByCaller(Exclusivity exclusivity) const {
  switch (exclusivity) {
    case EXCLUSIVE:
      ZC_ASSERT(futex & EXCLUSIVE_HELD, "Tried to call getAlreadyLocked*() but lock is not held.");
      break;
    case SHARED:
      ZC_ASSERT(futex & SHARED_COUNT_MASK,
                "Tried to call getAlreadyLocked*() but lock is not held.");
      break;
  }
}

void Mutex::wait(Predicate& predicate, Maybe<Duration> timeout, LockSourceLocationArg location) {
  // Add waiter to list.
  Waiter waiter{zc::none, waitersTail, predicate, zc::none, 0, timeout != zc::none};
  addWaiter(waiter);

  BlockedOnReason blockReason = BlockedOnCondVarWait{*this, &waiter, location};
  ZC_DEFER(setCurrentThreadIsNoLongerWaiting());

  // To guarantee that we've re-locked the mutex before scope exit, keep track of whether it is
  // currently.
  bool currentlyLocked = true;
  ZC_DEFER({
    // Infinite timeout for re-obtaining the lock is on purpose because the post-condition for this
    // function has to be that the lock state hasn't changed (& we have to be locked when we enter
    // since that's how condvars work).
    if (!currentlyLocked) lock(EXCLUSIVE, zc::none, location);
    removeWaiter(waiter);
  });

  if (!predicate.check()) {
    unlock(EXCLUSIVE, &waiter);
    currentlyLocked = false;

    struct timespec ts;
    struct timespec* tsp = nullptr;
    ZC_IF_SOME(t, timeout) {
      ts = toAbsoluteTimespec(now() + t);
      tsp = &ts;
    }

    setCurrentThreadIsWaitingFor(&blockReason);

    // Wait for someone to set our futex to 1.
    for (;;) {
      // Note we use FUTEX_WAIT_BITSET_PRIVATE + FUTEX_BITSET_MATCH_ANY to get the same effect as
      // FUTEX_WAIT_PRIVATE except that the timeout is specified as an absolute time based on
      // CLOCK_MONOTONIC. Otherwise, FUTEX_WAIT_PRIVATE interprets it as a relative time, forcing
      // us to recompute the time after every iteration.
      ZC_SYSCALL_HANDLE_ERRORS(syscall(SYS_futex, &waiter.futex, FUTEX_WAIT_BITSET_PRIVATE, 0, tsp,
                                       nullptr, FUTEX_BITSET_MATCH_ANY)) {
        case EAGAIN:
          // Indicates that the futex was already non-zero by the time the kernel looked at it.
          // Not an error.
          break;
        case ETIMEDOUT: {
          // Wait timed out. This leaves us in a bit of a pickle: Ownership of the mutex was not
          // transferred to us from another thread. So, we need to lock it ourselves. But, another
          // thread might be in the process of signaling us and transferring ownership. So, we
          // first must atomically take control of our destiny.
          ZC_ASSERT(timeout != zc::none);
          uint expected = 0;
          if (__atomic_compare_exchange_n(&waiter.futex, &expected, 1, false, __ATOMIC_ACQUIRE,
                                          __ATOMIC_ACQUIRE)) {
            // OK, we set our own futex to 1. That means no other thread will, and so we won't be
            // receiving a mutex ownership transfer. We have to lock the mutex ourselves.
            setCurrentThreadIsNoLongerWaiting();
            lock(EXCLUSIVE, zc::none, location);
            currentlyLocked = true;
            return;
          } else {
            // Oh, someone else actually did signal us, apparently. Let's move on as if the futex
            // call told us so.
            break;
          }
        }
        default:
          ZC_FAIL_SYSCALL("futex(FUTEX_WAIT_PRIVATE)", error);
      }

      setCurrentThreadIsNoLongerWaiting();

      if (__atomic_load_n(&waiter.futex, __ATOMIC_ACQUIRE)) {
        // We received a lock ownership transfer from another thread.
        currentlyLocked = true;

        // The other thread checked the predicate before the transfer.
#ifdef ZC_DEBUG
        assertLockedByCaller(EXCLUSIVE);
#endif

        ZC_IF_SOME(exception, waiter.exception) {
          // The predicate threw an exception, apparently. Propagate it.
          // TODO(someday): Could we somehow have this be a recoverable exception? Presumably we'd
          //   then want MutexGuarded::when() to skip calling the callback, but then what should it
          //   return, since it normally returns the callback's result? Or maybe people who disable
          //   exceptions just really should not write predicates that can throw.
          zc::throwFatalException(zc::mv(*exception));
        }

        return;
      }
    }
  }
}

void Mutex::induceSpuriousWakeupForTest() {
  auto nextWaiter = waitersHead;
  for (;;) {
    ZC_IF_SOME(waiter, nextWaiter) {
      nextWaiter = waiter.next;
      syscall(SYS_futex, &waiter.futex, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
    }
    else {
      // No more waiters.
      break;
    }
  }
}

uint Mutex::numReadersWaitingForTest() const {
  assertLockedByCaller(EXCLUSIVE);
  return futex & SHARED_COUNT_MASK;
}

void Once::runOnce(Initializer& init, LockSourceLocationArg location) {
startOver:
  uint state = UNINITIALIZED;
  if (__atomic_compare_exchange_n(&futex, &state, INITIALIZING, false, __ATOMIC_RELAXED,
                                  __ATOMIC_RELAXED)) {
    // It's our job to initialize!
    {
      ZC_ON_SCOPE_FAILURE({
        // An exception was thrown by the initializer.  We have to revert.
        if (__atomic_exchange_n(&futex, UNINITIALIZED, __ATOMIC_RELEASE) ==
            INITIALIZING_WITH_WAITERS) {
          // Someone was waiting for us to finish.
          syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
        }
      });

      init.run();
    }
    if (__atomic_exchange_n(&futex, INITIALIZED, __ATOMIC_RELEASE) == INITIALIZING_WITH_WAITERS) {
      // Someone was waiting for us to finish.
      syscall(SYS_futex, &futex, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
    }
  } else {
    BlockedOnReason blockReason = BlockedOnOnceInit{*this, location};
    ZC_DEFER(setCurrentThreadIsNoLongerWaiting());

    for (;;) {
      if (state == INITIALIZED) {
        break;
      } else if (state == INITIALIZING) {
        // Initialization is taking place in another thread.  Indicate that we're waiting.
        if (!__atomic_compare_exchange_n(&futex, &state, INITIALIZING_WITH_WAITERS, true,
                                         __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
          // State changed, retry.
          continue;
        }
      } else {
        ZC_DASSERT(state == INITIALIZING_WITH_WAITERS);
      }

      // Wait for initialization.
      setCurrentThreadIsWaitingFor(&blockReason);
      syscall(SYS_futex, &futex, FUTEX_WAIT_PRIVATE, INITIALIZING_WITH_WAITERS, nullptr, nullptr,
              0);
      state = __atomic_load_n(&futex, __ATOMIC_ACQUIRE);

      if (state == UNINITIALIZED) {
        // Oh hey, apparently whoever was trying to initialize gave up.  Let's take it from the
        // top.
        goto startOver;
      }
    }
  }
}

void Once::reset() {
  uint state = INITIALIZED;
  if (!__atomic_compare_exchange_n(&futex, &state, UNINITIALIZED, false, __ATOMIC_RELEASE,
                                   __ATOMIC_RELAXED)) {
    ZC_FAIL_REQUIRE("reset() called while not initialized.");
  }
}

#elif _WIN32 || __CYGWIN__
// =======================================================================================
// Win32 implementation

#define coercedSrwLock (*reinterpret_cast<SRWLOCK*>(&srwLock))
#define coercedInitOnce (*reinterpret_cast<INIT_ONCE*>(&initOnce))
#define coercedCondvar(var) (*reinterpret_cast<CONDITION_VARIABLE*>(&var))

Mutex::Mutex() {
  static_assert(sizeof(SRWLOCK) == sizeof(srwLock), "SRWLOCK is not a pointer?");
  InitializeSRWLock(&coercedSrwLock);
}
Mutex::~Mutex() {}

bool Mutex::lock(Exclusivity exclusivity, Maybe<Duration> timeout, NoopSourceLocation) {
  if (timeout != zc::none) {
    ZC_UNIMPLEMENTED("Locking a mutex with a timeout is only supported on Linux.");
  }
  switch (exclusivity) {
    case EXCLUSIVE:
      AcquireSRWLockExclusive(&coercedSrwLock);
      break;
    case SHARED:
      AcquireSRWLockShared(&coercedSrwLock);
      break;
  }
  return true;
}

void Mutex::wakeReadyWaiter(Waiter* waiterToSkip) {
  // Look for a waiter whose predicate is now evaluating true, and wake it. We wake no more than
  // one waiter because only one waiter could get the lock anyway, and once it releases that lock
  // it will awake the next waiter if necessary.

  auto nextWaiter = waitersHead;
  for (;;) {
    ZC_IF_SOME(waiter, nextWaiter) {
      nextWaiter = waiter.next;

      if (&waiter != waiterToSkip && checkPredicate(waiter)) {
        // This waiter's predicate now evaluates true, so wake it up. It doesn't matter if we
        // use Wake vs. WakeAll here since there's always only one thread waiting.
        WakeConditionVariable(&coercedCondvar(waiter.condvar));

        // We only need to wake one waiter. Note that unlike the futex-based implementation, we
        // cannot "transfer ownership" of the lock to the waiter, therefore we cannot guarantee
        // that the condition is still true when that waiter finally awakes. However, if the
        // condition is no longer true at that point, the waiter will re-check all other
        // waiters' conditions and possibly wake up any other waiter who is now ready, hence we
        // still only need to wake one waiter here.
        return;
      }
    }
    else {
      // No more waiters.
      break;
    }
  }
}

void Mutex::unlock(Exclusivity exclusivity, Waiter* waiterToSkip) {
  switch (exclusivity) {
    case EXCLUSIVE: {
      ZC_DEFER(ReleaseSRWLockExclusive(&coercedSrwLock));

      // Check if there are any conditional waiters. Note we only do this when unlocking an
      // exclusive lock since under a shared lock the state couldn't have changed.
      wakeReadyWaiter(waiterToSkip);
      break;
    }

    case SHARED:
      ReleaseSRWLockShared(&coercedSrwLock);
      break;
  }
}

void Mutex::assertLockedByCaller(Exclusivity exclusivity) const {
  // We could use TryAcquireSRWLock*() here like we do with the pthread version. However, as of
  // this writing, my version of Wine (1.6.2) doesn't implement these functions and will abort if
  // they are called. Since we were only going to use them as a hacky way to check if the lock is
  // held for debug purposes anyway, we just don't bother.
}

void Mutex::wait(Predicate& predicate, Maybe<Duration> timeout, NoopSourceLocation) {
  // Add waiter to list.
  Waiter waiter{zc::none, waitersTail, predicate, zc::none, 0};
  static_assert(sizeof(waiter.condvar) == sizeof(CONDITION_VARIABLE),
                "CONDITION_VARIABLE is not a pointer?");
  InitializeConditionVariable(&coercedCondvar(waiter.condvar));

  addWaiter(waiter);
  ZC_DEFER(removeWaiter(waiter));

  DWORD sleepMs;

  // Only initialized if `timeout` is non-null.
  const MonotonicClock* clock = nullptr;
  zc::Maybe<zc::TimePoint> endTime;

  ZC_IF_SOME(t, timeout) {
    // Windows sleeps are inaccurate -- they can be longer *or shorter* than the requested amount.
    // For many use cases of our API, a too-short sleep would be unacceptable. Experimentally, it
    // seems like sleeps can be up to half a millisecond short, so we'll add half a millisecond
    // (and then we round up, below).
    t += 500 * zc::MICROSECONDS;

    // Compute initial sleep time.
    sleepMs = t / zc::MILLISECONDS;
    if (t % zc::MILLISECONDS > 0 * zc::SECONDS) {
      // We guarantee we won't wake up too early.
      ++sleepMs;
    }

    clock = &systemPreciseMonotonicClock();
    endTime = clock->now() + t;
  }
  else { sleepMs = INFINITE; }

  while (!predicate.check()) {
    // SleepConditionVariableSRW() will temporarily release the lock, so we need to signal other
    // waiters that are now ready.
    wakeReadyWaiter(&waiter);

    if (SleepConditionVariableSRW(&coercedCondvar(waiter.condvar), &coercedSrwLock, sleepMs, 0)) {
      // Normal result. Continue loop to check predicate.
    } else {
      DWORD error = GetLastError();
      if (error == ERROR_TIMEOUT) {
        // Windows may have woken us up too early, so don't return yet. Instead, proceed through the
        // loop and rely on our sleep time recalculation to detect if we timed out.
      } else {
        ZC_FAIL_WIN32("SleepConditionVariableSRW()", error);
      }
    }

    ZC_IF_SOME(exception, waiter.exception) {
      // The predicate threw an exception, apparently. Propagate it.
      // TODO(someday): Could we somehow have this be a recoverable exception? Presumably we'd
      //   then want MutexGuarded::when() to skip calling the callback, but then what should it
      //   return, since it normally returns the callback's result? Or maybe people who disable
      //   exceptions just really should not write predicates that can throw.
      zc::throwFatalException(zc::mv(*exception));
    }

    // Recompute sleep time.
    ZC_IF_SOME(e, endTime) {
      auto now = clock->now();

      if (e > now) {
        auto sleepTime = e - now;
        sleepMs = sleepTime / zc::MILLISECONDS;
        if (sleepTime % zc::MILLISECONDS > 0 * zc::SECONDS) {
          // We guarantee we won't wake up too early.
          ++sleepMs;
        }
      } else {
        // Oops, already timed out.
        return;
      }
    }
  }
}

void Mutex::induceSpuriousWakeupForTest() {
  auto nextWaiter = waitersHead;
  for (;;) {
    ZC_IF_SOME(waiter, nextWaiter) {
      nextWaiter = waiter.next;
      WakeConditionVariable(&coercedCondvar(waiter.condvar));
    }
    else {
      // No more waiters.
      break;
    }
  }
}

static BOOL WINAPI nullInitializer(PINIT_ONCE initOnce, PVOID parameter, PVOID* context) {
  return true;
}

Once::Once(bool startInitialized) {
  static_assert(sizeof(INIT_ONCE) == sizeof(initOnce), "INIT_ONCE is not a pointer?");
  InitOnceInitialize(&coercedInitOnce);
  if (startInitialized) {
    InitOnceExecuteOnce(&coercedInitOnce, &nullInitializer, nullptr, nullptr);
  }
}
Once::~Once() {}

void Once::runOnce(Initializer& init, NoopSourceLocation) {
  BOOL needInit;
  while (!InitOnceBeginInitialize(&coercedInitOnce, 0, &needInit, nullptr)) {
    // Init was occurring in another thread, but then failed with an exception. Retry.
  }

  if (needInit) {
    {
      ZC_ON_SCOPE_FAILURE(InitOnceComplete(&coercedInitOnce, INIT_ONCE_INIT_FAILED, nullptr));
      init.run();
    }

    ZC_ASSERT(InitOnceComplete(&coercedInitOnce, 0, nullptr));
  }
}

bool Once::isInitialized() noexcept {
  BOOL junk;
  return InitOnceBeginInitialize(&coercedInitOnce, INIT_ONCE_CHECK_ONLY, &junk, nullptr);
}

void Once::reset() { InitOnceInitialize(&coercedInitOnce); }

#else
// =======================================================================================
// Generic pthreads-based implementation

#define ZC_PTHREAD_CALL(code)                                        \
  {                                                                  \
    int pthreadError = code;                                         \
    if (pthreadError != 0) { ZC_FAIL_SYSCALL(#code, pthreadError); } \
  }

#define ZC_PTHREAD_CLEANUP(code)                                             \
  {                                                                          \
    int pthreadError = code;                                                 \
    if (pthreadError != 0) { ZC_LOG(ERROR, #code, strerror(pthreadError)); } \
  }

Mutex::Mutex() : mutex(PTHREAD_RWLOCK_INITIALIZER) {
#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070
  // In older versions of MacOS, mutexes initialized statically cannot be destroyed,
  // so we must call the init function.
  ZC_PTHREAD_CALL(pthread_rwlock_init(&mutex, NULL));
#endif
}
Mutex::~Mutex() { ZC_PTHREAD_CLEANUP(pthread_rwlock_destroy(&mutex)); }

bool Mutex::lock(Exclusivity exclusivity, Maybe<Duration> timeout, NoopSourceLocation) {
  if (timeout != zc::none) {
    ZC_UNIMPLEMENTED("Locking a mutex with a timeout is only supported on Linux.");
  }
  switch (exclusivity) {
    case EXCLUSIVE:
      ZC_PTHREAD_CALL(pthread_rwlock_wrlock(&mutex));
      break;
    case SHARED:
      ZC_PTHREAD_CALL(pthread_rwlock_rdlock(&mutex));
      break;
  }
  return true;
}

void Mutex::unlock(Exclusivity exclusivity, Waiter* waiterToSkip) {
  ZC_DEFER(ZC_PTHREAD_CALL(pthread_rwlock_unlock(&mutex)));

  if (exclusivity == EXCLUSIVE) {
    // Check if there are any conditional waiters. Note we only do this when unlocking an
    // exclusive lock since under a shared lock the state couldn't have changed.
    auto nextWaiter = waitersHead;
    for (;;) {
      ZC_IF_SOME(waiter, nextWaiter) {
        nextWaiter = waiter.next;

        if (&waiter != waiterToSkip && checkPredicate(waiter)) {
          // This waiter's predicate now evaluates true, so wake it up. It doesn't matter if we
          // use _signal() vs. _broadcast() here since there's always only one thread waiting.
          ZC_PTHREAD_CALL(pthread_mutex_lock(&waiter.stupidMutex));
          ZC_PTHREAD_CALL(pthread_cond_signal(&waiter.condvar));
          ZC_PTHREAD_CALL(pthread_mutex_unlock(&waiter.stupidMutex));

          // We only need to wake one waiter. Note that unlike the futex-based implementation, we
          // cannot "transfer ownership" of the lock to the waiter, therefore we cannot guarantee
          // that the condition is still true when that waiter finally awakes. However, if the
          // condition is no longer true at that point, the waiter will re-check all other waiters'
          // conditions and possibly wake up any other waiter who is now ready, hence we still only
          // need to wake one waiter here.
          break;
        }
      }
      else {
        // No more waiters.
        break;
      }
    }
  }
}

void Mutex::assertLockedByCaller(Exclusivity exclusivity) const {
  switch (exclusivity) {
    case EXCLUSIVE:
      // A read lock should fail if the mutex is already held for writing.
      if (pthread_rwlock_tryrdlock(&mutex) == 0) {
        pthread_rwlock_unlock(&mutex);
        ZC_FAIL_ASSERT("Tried to call getAlreadyLocked*() but lock is not held.");
      }
      break;
    case SHARED:
      // A write lock should fail if the mutex is already held for reading or writing.  We don't
      // have any way to prove that the lock is held only for reading.
      if (pthread_rwlock_trywrlock(&mutex) == 0) {
        pthread_rwlock_unlock(&mutex);
        ZC_FAIL_ASSERT("Tried to call getAlreadyLocked*() but lock is not held.");
      }
      break;
  }
}

void Mutex::wait(Predicate& predicate, Maybe<Duration> timeout, NoopSourceLocation) {
  // Add waiter to list.
  Waiter waiter{zc::none,
                waitersTail,
                predicate,
                zc::none,
                PTHREAD_COND_INITIALIZER,
                PTHREAD_MUTEX_INITIALIZER};

#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070
  // In older versions of MacOS, mutexes initialized statically cannot be destroyed,
  // so we must call the init function.
  ZC_PTHREAD_CALL(pthread_cond_init(&waiter.condvar, NULL));
  ZC_PTHREAD_CALL(pthread_mutex_init(&waiter.stupidMutex, NULL));
#endif

  addWaiter(waiter);

  // To guarantee that we've re-locked the mutex before scope exit, keep track of whether it is
  // currently.
  bool currentlyLocked = true;
  ZC_DEFER({
    if (!currentlyLocked) lock(EXCLUSIVE, zc::none, NoopSourceLocation{});
    removeWaiter(waiter);

    // Destroy pthread objects.
    ZC_PTHREAD_CLEANUP(pthread_mutex_destroy(&waiter.stupidMutex));
    ZC_PTHREAD_CLEANUP(pthread_cond_destroy(&waiter.condvar));
  });

#if !__APPLE__
  if (timeout != zc::none) {
    // Oops, the default condvar uses the wall clock, which is dumb... fix it to use the monotonic
    // clock. (Except not on macOS, where pthread_condattr_setclock() is unimplemented, but there's
    // a bizarre pthread_cond_timedwait_relative_np() method we can use instead...)
    pthread_condattr_t attr;
    ZC_PTHREAD_CALL(pthread_condattr_init(&attr));
    ZC_PTHREAD_CALL(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    pthread_cond_init(&waiter.condvar, &attr);
    ZC_PTHREAD_CALL(pthread_condattr_destroy(&attr));
  }
#endif

  Maybe<struct timespec> endTime =
      timeout.map([](Duration d) { return toAbsoluteTimespec(now() + d); });

  while (!predicate.check()) {
    // pthread condvars only work with basic mutexes, not rwlocks. So, we need to lock a basic
    // mutex before we unlock the real mutex, and the signaling thread also needs to lock this
    // mutex, in order to ensure that this thread is actually waiting on the condvar before it is
    // signaled.
    ZC_PTHREAD_CALL(pthread_mutex_lock(&waiter.stupidMutex));

    // OK, now we can unlock the main mutex.
    unlock(EXCLUSIVE, &waiter);
    currentlyLocked = false;

    bool timedOut = false;

    // Wait for someone to signal the condvar.
    ZC_IF_SOME(t, endTime) {
#if __APPLE__
      // On macOS, the absolute timeout can only be specified in wall time, not monotonic time,
      // which means modifying the system clock will break the wait. However, macOS happens to
      // provide an alternative relative-time wait function, so I guess we'll use that. It does
      // require recomputing the time every iteration...
      struct timespec ts = toRelativeTimespec(zc::max(toTimePoint(t) - now(), 0 * zc::SECONDS));
      int error = pthread_cond_timedwait_relative_np(&waiter.condvar, &waiter.stupidMutex, &ts);
#else
      int error = pthread_cond_timedwait(&waiter.condvar, &waiter.stupidMutex, &t);
#endif
      if (error != 0) {
        if (error == ETIMEDOUT) {
          timedOut = true;
        } else {
          ZC_FAIL_SYSCALL("pthread_cond_timedwait", error);
        }
      }
    }
    else { ZC_PTHREAD_CALL(pthread_cond_wait(&waiter.condvar, &waiter.stupidMutex)); }

    // We have to be very careful about lock ordering here. We need to unlock stupidMutex before
    // re-locking the main mutex, because another thread may have a lock on the main mutex already
    // and be waiting for a lock on stupidMutex. Note that other thread may signal the condvar
    // right after we unlock stupidMutex but before we re-lock the main mutex. That is fine,
    // because we've already been signaled.
    ZC_PTHREAD_CALL(pthread_mutex_unlock(&waiter.stupidMutex));

    lock(EXCLUSIVE, zc::none, NoopSourceLocation{});
    currentlyLocked = true;

    ZC_IF_SOME(exception, waiter.exception) {
      // The predicate threw an exception, apparently. Propagate it.
      // TODO(someday): Could we somehow have this be a recoverable exception? Presumably we'd
      //   then want MutexGuarded::when() to skip calling the callback, but then what should it
      //   return, since it normally returns the callback's result? Or maybe people who disable
      //   exceptions just really should not write predicates that can throw.
      zc::throwFatalException(zc::mv(*exception));
    }

    if (timedOut) { return; }
  }
}

void Mutex::induceSpuriousWakeupForTest() {
  auto nextWaiter = waitersHead;
  for (;;) {
    ZC_IF_SOME(waiter, nextWaiter) {
      nextWaiter = waiter.next;
      ZC_PTHREAD_CALL(pthread_mutex_lock(&waiter.stupidMutex));
      ZC_PTHREAD_CALL(pthread_cond_signal(&waiter.condvar));
      ZC_PTHREAD_CALL(pthread_mutex_unlock(&waiter.stupidMutex));
    }
    else {
      // No more waiters.
      break;
    }
  }
}

Once::Once(bool startInitialized)
    : state(startInitialized ? INITIALIZED : UNINITIALIZED), mutex(PTHREAD_MUTEX_INITIALIZER) {
#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070
  // In older versions of MacOS, mutexes initialized statically cannot be destroyed,
  // so we must call the init function.
  ZC_PTHREAD_CALL(pthread_mutex_init(&mutex, NULL));
#endif
}
Once::~Once() { ZC_PTHREAD_CLEANUP(pthread_mutex_destroy(&mutex)); }

void Once::runOnce(Initializer& init, NoopSourceLocation) {
  ZC_PTHREAD_CALL(pthread_mutex_lock(&mutex));
  ZC_DEFER(ZC_PTHREAD_CALL(pthread_mutex_unlock(&mutex)));

  if (state != UNINITIALIZED) { return; }

  init.run();

  __atomic_store_n(&state, INITIALIZED, __ATOMIC_RELEASE);
}

void Once::reset() {
  State oldState = INITIALIZED;
  if (!__atomic_compare_exchange_n(&state, &oldState, UNINITIALIZED, false, __ATOMIC_RELEASE,
                                   __ATOMIC_RELAXED)) {
    ZC_FAIL_REQUIRE("reset() called while not initialized.");
  }
}

#endif

}  // namespace _
}  // namespace zc
