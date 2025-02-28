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

#pragma once

#include <inttypes.h>

#include "zc/core/memory.h"
#include "zc/core/source-location.h"
#include "zc/core/time.h"

ZC_BEGIN_HEADER

#if __linux__ && !defined(ZC_USE_FUTEX)
#define ZC_USE_FUTEX 1
#endif

#if !ZC_USE_FUTEX && !_WIN32 && !__CYGWIN__
// We fall back to pthreads when we don't have a better platform-specific primitive. pthreads
// mutexes are bloated, though, so we like to avoid them. Hence on Linux we use futex(), and on
// Windows we use SRW locks and friends. On Cygwin we prefer the Win32 primitives both because they
// are more efficient and because I ran into problems with Cygwin's implementation of RW locks
// seeming to allow multiple threads to lock the same mutex (but I didn't investigate very
// closely).
//
// TODO(someday):  Write efficient low-level locking primitives for other platforms.
#include <pthread.h>
#endif

namespace zc {

class Exception;

using LockSourceLocation = NoopSourceLocation;
using LockSourceLocationArg = NoopSourceLocation;

// =======================================================================================
// Private details -- public interfaces follow below.

namespace _ {  // private
class Mutex {
  // Internal implementation details.  See `MutexGuarded<T>`.

  struct Waiter;

public:
  Mutex();
  ~Mutex();
  ZC_DISALLOW_COPY_AND_MOVE(Mutex);

  enum Exclusivity { EXCLUSIVE, SHARED };

  bool lock(Exclusivity exclusivity, Maybe<Duration> timeout, LockSourceLocationArg location);
  void unlock(Exclusivity exclusivity, Waiter* waiterToSkip = nullptr);

  void assertLockedByCaller(Exclusivity exclusivity) const;
  // In debug mode, assert that the mutex is locked by the calling thread, or if that is
  // non-trivial, assert that the mutex is locked (which should be good enough to catch problems
  // in unit tests).  In non-debug builds, do nothing.

  class Predicate {
  public:
    virtual bool check() = 0;
  };

  void wait(Predicate& predicate, Maybe<Duration> timeout, LockSourceLocationArg location);
  // If predicate.check() returns false, unlock the mutex until predicate.check() returns true, or
  // when the timeout (if any) expires. The mutex is always re-locked when this returns regardless
  // of whether the timeout expired, and including if it throws.
  //
  // Requires that the mutex is already exclusively locked before calling.

  void induceSpuriousWakeupForTest();
  // Utility method for mutex-test.c++ which causes a spurious thread wakeup on all threads that
  // are waiting for a wait() condition. Assuming correct implementation, all those threads
  // should immediately go back to sleep.

#if ZC_USE_FUTEX
  uint numReadersWaitingForTest() const;
  // The number of reader locks that are currently blocked on this lock (must be called while
  // holding the writer lock). This is really only a utility method for mutex-test.c++ so it can
  // validate certain invariants.
#endif

private:
#if ZC_USE_FUTEX
  uint futex;
  // bit 31 (msb) = set if exclusive lock held
  // bit 30 (msb) = set if threads are waiting for exclusive lock
  // bits 0-29 = count of readers; If an exclusive lock is held, this is the count of threads
  //   waiting for a read lock, otherwise it is the count of threads that currently hold a read
  //   lock.

#ifdef ZC_CONTENTION_WARNING_THRESHOLD
  bool printContendedReader = false;
#endif

  static constexpr uint EXCLUSIVE_HELD = 1u << 31;
  static constexpr uint EXCLUSIVE_REQUESTED = 1u << 30;
  static constexpr uint SHARED_COUNT_MASK = EXCLUSIVE_REQUESTED - 1;

#elif _WIN32 || __CYGWIN__
  uintptr_t srwLock;  // Actually an SRWLOCK, but don't want to #include <windows.h> in header.

#else
  mutable pthread_rwlock_t mutex;
#endif

  struct Waiter {
    zc::Maybe<Waiter&> next;
    zc::Maybe<Waiter&>* prev;
    Predicate& predicate;
    Maybe<Own<Exception>> exception;
#if ZC_USE_FUTEX
    uint futex;
    bool hasTimeout;
#elif _WIN32 || __CYGWIN__
    uintptr_t condvar;
    // Actually CONDITION_VARIABLE, but don't want to #include <windows.h> in header.
#else
    pthread_cond_t condvar;

    pthread_mutex_t stupidMutex;
    // pthread condvars are only compatible with basic pthread mutexes, not rwlocks, for no
    // particularly good reason. To work around this, we need an extra mutex per condvar.
#endif
  };

  zc::Maybe<Waiter&> waitersHead = zc::none;
  zc::Maybe<Waiter&>* waitersTail = &waitersHead;
  // linked list of waiters; can only modify under lock

  inline void addWaiter(Waiter& waiter);
  inline void removeWaiter(Waiter& waiter);
  bool checkPredicate(Waiter& waiter);
#if _WIN32 || __CYGWIN__
  void wakeReadyWaiter(Waiter* waiterToSkip);
#endif
};

class Once {
  // Internal implementation details.  See `Lazy<T>`.

public:
#if ZC_USE_FUTEX
  inline Once(bool startInitialized = false)
      : futex(startInitialized ? INITIALIZED : UNINITIALIZED) {}
#else
  Once(bool startInitialized = false);
  ~Once();
#endif
  ZC_DISALLOW_COPY_AND_MOVE(Once);

  class Initializer {
  public:
    virtual void run() = 0;
  };

  void runOnce(Initializer& init, LockSourceLocationArg location);

#if _WIN32 || __CYGWIN__  // TODO(perf): Can we make this inline on win32 somehow?
  bool isInitialized() noexcept;

#else
  inline bool isInitialized() noexcept {
    // Fast path check to see if runOnce() would simply return immediately.
#if ZC_USE_FUTEX
    return __atomic_load_n(&futex, __ATOMIC_ACQUIRE) == INITIALIZED;
#else
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == INITIALIZED;
#endif
  }
#endif

  void reset();
  // Returns the state from initialized to uninitialized.  It is an error to call this when
  // not already initialized, or when runOnce() or isInitialized() might be called concurrently in
  // another thread.

private:
#if ZC_USE_FUTEX
  uint futex;

  enum State { UNINITIALIZED, INITIALIZING, INITIALIZING_WITH_WAITERS, INITIALIZED };

#elif _WIN32 || __CYGWIN__
  uintptr_t initOnce;  // Actually an INIT_ONCE, but don't want to #include <windows.h> in header.

#else
  enum State { UNINITIALIZED, INITIALIZED };
  State state;
  pthread_mutex_t mutex;
#endif
};

}  // namespace _

// =======================================================================================
// Public interface

template <typename T>
class Locked {
  // Return type for `MutexGuarded<T>::lock()`.  `Locked<T>` provides access to the bounded object
  // and unlocks the mutex when it goes out of scope.

public:
  ZC_DISALLOW_COPY(Locked);
  inline Locked() : mutex(nullptr), ptr(nullptr) {}
  inline Locked(Locked&& other) : mutex(other.mutex), ptr(other.ptr) {
    other.mutex = nullptr;
    other.ptr = nullptr;
  }
  inline ~Locked() {
    if (mutex != nullptr) mutex->unlock(isConst<T>() ? _::Mutex::SHARED : _::Mutex::EXCLUSIVE);
  }

  inline Locked& operator=(Locked&& other) {
    if (mutex != nullptr) mutex->unlock(isConst<T>() ? _::Mutex::SHARED : _::Mutex::EXCLUSIVE);
    mutex = other.mutex;
    ptr = other.ptr;
    other.mutex = nullptr;
    other.ptr = nullptr;
    return *this;
  }

  inline void release() {
    if (mutex != nullptr) mutex->unlock(isConst<T>() ? _::Mutex::SHARED : _::Mutex::EXCLUSIVE);
    mutex = nullptr;
    ptr = nullptr;
  }

  inline T* operator->() { return ptr; }
  inline const T* operator->() const { return ptr; }
  inline T& operator*() { return *ptr; }
  inline const T& operator*() const { return *ptr; }
  inline T* get() { return ptr; }
  inline const T* get() const { return ptr; }
  inline operator T*() { return ptr; }
  inline operator const T*() const { return ptr; }

  template <typename Cond>
  void wait(Cond&& condition, Maybe<Duration> timeout = zc::none,
            LockSourceLocationArg location = {}) {
    // Unlocks the lock until `condition(state)` evaluates true (where `state` is type `const T&`
    // referencing the object protected by the lock).

    // We can't wait on a shared lock because the internal bookkeeping needed for a wait requires
    // the protection of an exclusive lock.
    static_assert(!isConst<T>(), "cannot wait() on shared lock");

    struct PredicateImpl final : public _::Mutex::Predicate {
      bool check() override { return condition(value); }

      Cond&& condition;
      const T& value;

      PredicateImpl(Cond&& condition, const T& value)
          : condition(zc::fwd<Cond>(condition)), value(value) {}
    };

    PredicateImpl impl(zc::fwd<Cond>(condition), *ptr);
    mutex->wait(impl, timeout, location);
  }

private:
  _::Mutex* mutex;
  T* ptr;

  inline Locked(_::Mutex& mutex, T& value) : mutex(&mutex), ptr(&value) {}

  template <typename U>
  friend class MutexGuarded;
  template <typename U>
  friend class ExternalMutexGuarded;

#if ZC_MUTEX_TEST
public:
#endif
  void induceSpuriousWakeupForTest() { mutex->induceSpuriousWakeupForTest(); }
  // Utility method for mutex-test.c++ which causes a spurious thread wakeup on all threads that
  // are waiting for a when() condition. Assuming correct implementation, all those threads should
  // immediately go back to sleep.
};

template <typename T>
class MutexGuarded {
  // An object of type T, bounded by a mutex.  In order to access the object, you must lock it.
  //
  // Write locks are not "recursive" -- trying to lock again in a thread that already holds a lock
  // will deadlock.  Recursive write locks are usually a sign of bad design.
  //
  // Unfortunately, **READ LOCKS ARE NOT RECURSIVE** either.  Common sense says they should be.
  // But on many operating systems (BSD, OSX), recursively read-locking a pthread_rwlock is
  // actually unsafe.  The problem is that writers are "prioritized" over readers, so a read lock
  // request will block if any write lock requests are outstanding.  So, if thread A takes a read
  // lock, thread B requests a write lock (and starts waiting), and then thread A tries to take
  // another read lock recursively, the result is deadlock.

public:
  template <typename... Params>
  explicit MutexGuarded(Params&&... params);
  // Initialize the mutex-bounded object by passing the given parameters to its constructor.

  Locked<T> lockExclusive(LockSourceLocationArg location = {}) const;
  // Exclusively locks the object and returns it.  The returned `Locked<T>` can be passed by
  // move, similar to `Own<T>`.
  //
  // This method is declared `const` in accordance with ZC style rules which say that constness
  // should be used to indicate thread-safety.  It is safe to share a const pointer between threads,
  // but it is not safe to share a mutable pointer.  Since the whole point of MutexGuarded is to
  // be shared between threads, its methods should be const, even though locking it produces a
  // non-const pointer to the contained object.

  Locked<const T> lockShared(LockSourceLocationArg location = {}) const;
  // Lock the value for shared access.  Multiple shared locks can be taken concurrently, but cannot
  // be held at the same time as a non-shared lock.

  Maybe<Locked<T>> lockExclusiveWithTimeout(Duration timeout,
                                            LockSourceLocationArg location = {}) const;
  // Attempts to exclusively lock the object. If the timeout elapses before the lock is acquired,
  // this returns null.

  Maybe<Locked<const T>> lockSharedWithTimeout(Duration timeout,
                                               LockSourceLocationArg location = {}) const;
  // Attempts to lock the value for shared access. If the timeout elapses before the lock is
  // acquired, this returns null.

  inline const T& getWithoutLock() const { return value; }
  inline T& getWithoutLock() { return value; }
  // Escape hatch for cases where some external factor guarantees that it's safe to get the
  // value.  You should treat these like const_cast -- be highly suspicious of any use.

  inline const T& getAlreadyLockedShared() const;
  inline T& getAlreadyLockedShared();
  inline T& getAlreadyLockedExclusive() const;
  // Like `getWithoutLock()`, but asserts that the lock is already held by the calling thread.

  template <typename Cond, typename Func>
  auto when(Cond&& condition, Func&& callback, Maybe<Duration> timeout = zc::none,
            LockSourceLocationArg location = {}) const -> decltype(callback(instance<T&>())) {
    // Waits until condition(state) returns true, then calls callback(state) under lock.
    //
    // `condition`, when called, receives as its parameter a const reference to the state, which is
    // locked (either shared or exclusive). `callback` receives a mutable reference, which is
    // exclusively locked.
    //
    // `condition()` may be called multiple times, from multiple threads, while waiting for the
    // condition to become true. It may even return true once, but then be called more times.
    // It is guaranteed, though, that at the time `callback()` is finally called, `condition()`
    // would currently return true (assuming it is a pure function of the guarded data).
    //
    // If `timeout` is specified, then after the given amount of time, the callback will be called
    // regardless of whether the condition is true. In this case, when `callback()` is called,
    // `condition()` may in fact evaluate false, but *only* if the timeout was reached.
    //
    // TODO(cleanup): lock->wait() is a better interface. Can we deprecate this one?

    auto lock = lockExclusive();
    lock.wait(zc::fwd<Cond>(condition), timeout, location);
    return callback(value);
  }

private:
  mutable _::Mutex mutex;
  mutable T value;
};

template <typename T>
class MutexGuarded<const T> {
  // MutexGuarded cannot guard a const type.  This would be pointless anyway, and would complicate
  // the implementation of Locked<T>, which uses constness to decide what kind of lock it holds.
  static_assert(sizeof(T) < 0, "MutexGuarded's type cannot be const.");
};

template <typename T>
class ExternalMutexGuarded {
  // Holds a value that can only be manipulated while some other mutex is locked.
  //
  // The ExternalMutexGuarded<T> lives *outside* the scope of any lock on the mutex, but ensures
  // that the value it holds can only be accessed under lock by forcing the caller to present a
  // lock before accessing the value.
  //
  // Additionally, ExternalMutexGuarded<T>'s destructor will take an exclusive lock on the mutex
  // while destroying the held value, unless the value has been release()ed before hand.
  //
  // The type T must have the following properties (which probably all movable types satisfy):
  // - T is movable.
  // - Immediately after any of the following has happened, T's destructor is effectively a no-op
  //   (hence certainly not requiring locks):
  //   - The value has been default-constructed.
  //   - The value has been initialized by-move from a default-constructed T.
  //   - The value has been moved away.
  // - If ExternalMutexGuarded<T> is ever moved, then T must have a move constructor and move
  //   assignment operator that do not follow any pointers, therefore do not need to take a lock.
public:
  ExternalMutexGuarded(LockSourceLocationArg location = {}) : location(location) {}

  template <typename U, typename... Params>
  ExternalMutexGuarded(Locked<U> lock, Params&&... params, LockSourceLocationArg location = {})
      : mutex(lock.mutex), value(zc::fwd<Params>(params)...), location(location) {}
  // Construct the value in-place. This constructor requires passing ownership of the lock into
  // the constructor. Normally this should be a lock that you take on the line calling the
  // constructor, like:
  //
  //     ExternalMutexGuarded<T> foo(someMutexGuarded.lockExclusive());
  //
  // The reason this constructor does not accept an lvalue reference to an existing lock is because
  // this would be deadlock-prone: If an exception were thrown immediately after the constructor
  // completed, then the destructor would deadlock, because the lock would still be held. An
  // ExternalMutexGuarded must live outside the scope of any locks to avoid such a deadlock.

  ~ExternalMutexGuarded() noexcept(false) {
    if (mutex != nullptr) {
      mutex->lock(_::Mutex::EXCLUSIVE, zc::none, location);
      ZC_DEFER(mutex->unlock(_::Mutex::EXCLUSIVE));
      value = T();
    }
  }

  ExternalMutexGuarded(ExternalMutexGuarded&& other)
      : mutex(other.mutex), value(zc::mv(other.value)), location(other.location) {
    other.mutex = nullptr;
  }
  ExternalMutexGuarded& operator=(ExternalMutexGuarded&& other) {
    mutex = other.mutex;
    value = zc::mv(other.value);
    location = other.location;
    other.mutex = nullptr;
    return *this;
  }

  template <typename U>
  void set(Locked<U>& lock, T&& newValue) {
    ZC_IREQUIRE(mutex == nullptr);
    mutex = lock.mutex;
    value = zc::mv(newValue);
  }

  template <typename U>
  T& get(Locked<U>& lock) {
    ZC_IREQUIRE(lock.mutex == mutex);
    return value;
  }

  template <typename U>
  const T& get(Locked<const U>& lock) const {
    ZC_IREQUIRE(lock.mutex == mutex);
    return value;
  }

  template <typename U>
  T release(Locked<U>& lock) {
    // Release (move away) the value. This allows the destructor to skip locking the mutex.
    ZC_IREQUIRE(lock.mutex == mutex);
    T result = zc::mv(value);
    mutex = nullptr;
    return result;
  }

private:
  _::Mutex* mutex = nullptr;
  T value;
  ZC_NO_UNIQUE_ADDRESS LockSourceLocation location;
  // When built against C++20 (or clang >= 9.0), the overhead of this is elided. Otherwise this
  // struct will be 1 byte larger than it would otherwise be.
};

template <typename T>
class Lazy {
  // A lazily-initialized value.

public:
  template <typename Func>
  T& get(Func&& init, LockSourceLocationArg location = {});
  template <typename Func>
  const T& get(Func&& init, LockSourceLocationArg location = {}) const;
  // The first thread to call get() will invoke the given init function to construct the value.
  // Other threads will block until construction completes, then return the same value.
  //
  // `init` is a functor(typically a lambda) which takes `SpaceFor<T>&` as its parameter and returns
  // `Own<T>`.  If `init` throws an exception, the exception is propagated out of that thread's
  // call to `get()`, and subsequent calls behave as if `get()` hadn't been called at all yet --
  // in other words, subsequent calls retry initialization until it succeeds.

private:
  mutable _::Once once;
  mutable SpaceFor<T> space;
  mutable Own<T> value;

  template <typename Func>
  class InitImpl;
};

// =======================================================================================
// Inline implementation details

template <typename T>
template <typename... Params>
inline MutexGuarded<T>::MutexGuarded(Params&&... params) : value(zc::fwd<Params>(params)...) {}

template <typename T>
inline Locked<T> MutexGuarded<T>::lockExclusive(LockSourceLocationArg location) const {
  mutex.lock(_::Mutex::EXCLUSIVE, zc::none, location);
  return Locked<T>(mutex, value);
}

template <typename T>
inline Locked<const T> MutexGuarded<T>::lockShared(LockSourceLocationArg location) const {
  mutex.lock(_::Mutex::SHARED, zc::none, location);
  return Locked<const T>(mutex, value);
}

template <typename T>
inline Maybe<Locked<T>> MutexGuarded<T>::lockExclusiveWithTimeout(
    Duration timeout, LockSourceLocationArg location) const {
  if (mutex.lock(_::Mutex::EXCLUSIVE, timeout, location)) {
    return Locked<T>(mutex, value);
  } else {
    return zc::none;
  }
}

template <typename T>
inline Maybe<Locked<const T>> MutexGuarded<T>::lockSharedWithTimeout(
    Duration timeout, LockSourceLocationArg location) const {
  if (mutex.lock(_::Mutex::SHARED, timeout, location)) {
    return Locked<const T>(mutex, value);
  } else {
    return zc::none;
  }
}

template <typename T>
inline const T& MutexGuarded<T>::getAlreadyLockedShared() const {
#ifdef ZC_DEBUG
  mutex.assertLockedByCaller(_::Mutex::SHARED);
#endif
  return value;
}
template <typename T>
inline T& MutexGuarded<T>::getAlreadyLockedShared() {
#ifdef ZC_DEBUG
  mutex.assertLockedByCaller(_::Mutex::SHARED);
#endif
  return value;
}
template <typename T>
inline T& MutexGuarded<T>::getAlreadyLockedExclusive() const {
#ifdef ZC_DEBUG
  mutex.assertLockedByCaller(_::Mutex::EXCLUSIVE);
#endif
  return const_cast<T&>(value);
}

template <typename T>
template <typename Func>
class Lazy<T>::InitImpl : public _::Once::Initializer {
public:
  inline InitImpl(const Lazy<T>& lazy, Func&& func) : lazy(lazy), func(zc::fwd<Func>(func)) {}

  void run() override { lazy.value = func(lazy.space); }

private:
  const Lazy<T>& lazy;
  Func func;
};

template <typename T>
template <typename Func>
inline T& Lazy<T>::get(Func&& init, LockSourceLocationArg location) {
  if (!once.isInitialized()) {
    InitImpl<Func> initImpl(*this, zc::fwd<Func>(init));
    once.runOnce(initImpl, location);
  }
  return *value;
}

template <typename T>
template <typename Func>
inline const T& Lazy<T>::get(Func&& init, LockSourceLocationArg location) const {
  if (!once.isInitialized()) {
    InitImpl<Func> initImpl(*this, zc::fwd<Func>(init));
    once.runOnce(initImpl, location);
  }
  return *value;
}

}  // namespace zc

ZC_END_HEADER
