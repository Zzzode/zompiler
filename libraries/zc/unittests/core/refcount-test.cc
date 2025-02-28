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

#include "zc/core/refcount.h"

#include <zc/ztest/gtest.h>

namespace zc {

struct SetTrueInDestructor : public Refcounted, EnableAddRefToThis<SetTrueInDestructor> {
  SetTrueInDestructor(bool* ptr) : ptr(ptr) {}
  ~SetTrueInDestructor() { *ptr = true; }

  zc::Rc<SetTrueInDestructor> newRef() { return addRefToThis(); }

  bool* ptr;
};

TEST(Refcount, Basic) {
  bool b = false;
  Own<SetTrueInDestructor> ref1 = zc::refcounted<SetTrueInDestructor>(&b);
  EXPECT_FALSE(ref1->isShared());
  Own<SetTrueInDestructor> ref2 = zc::addRef(*ref1);
  EXPECT_TRUE(ref1->isShared());
  Own<SetTrueInDestructor> ref3 = zc::addRef(*ref2);
  EXPECT_TRUE(ref1->isShared());

  EXPECT_FALSE(b);
  ref1 = Own<SetTrueInDestructor>();
  EXPECT_TRUE(ref2->isShared());
  EXPECT_FALSE(b);
  ref3 = Own<SetTrueInDestructor>();
  EXPECT_FALSE(ref2->isShared());
  EXPECT_FALSE(b);
  ref2 = Own<SetTrueInDestructor>();
  EXPECT_TRUE(b);

#ifdef ZC_DEBUG
  b = false;
  SetTrueInDestructor obj(&b);
  EXPECT_ANY_THROW(addRef(obj));
#endif
}

ZC_TEST("Rc") {
  bool b = false;

  Rc<SetTrueInDestructor> ref1 = zc::rc<SetTrueInDestructor>(&b);
  EXPECT_FALSE(ref1->isShared());
  EXPECT_TRUE(ref1 != nullptr);
  EXPECT_FALSE(ref1 == nullptr);

  Rc<SetTrueInDestructor> ref2 = ref1.addRef();
  EXPECT_TRUE(ref1->isShared());
  EXPECT_TRUE(ref1 == ref2);

  {
    Rc<SetTrueInDestructor> ref3 = ref2.addRef();
    EXPECT_TRUE(ref3->isShared());
    // ref3 is dropped
  }

  EXPECT_FALSE(b);

  // start dropping references one by one

  EXPECT_TRUE(ref2->isShared());
  ref1 = nullptr;
  EXPECT_TRUE(ref1 == nullptr);
  EXPECT_FALSE(ref2->isShared());
  EXPECT_FALSE(b);
  EXPECT_FALSE(ref1 == ref2);

  ref2 = nullptr;
  EXPECT_TRUE(ref1 == ref2);

  // last reference dropped, SetTrueInDestructor destructor should execute
  EXPECT_TRUE(b);
}

ZC_TEST("Rc Own interop") {
  bool b = false;

  Rc<SetTrueInDestructor> ref1 = zc::rc<SetTrueInDestructor>(&b);

  EXPECT_FALSE(b);
  auto own = ref1.toOwn();
  EXPECT_TRUE(ref1 == nullptr);
  EXPECT_TRUE(own.get() != nullptr);

  EXPECT_FALSE(b);
  own = nullptr;
  EXPECT_TRUE(b);
}

struct Child : public SetTrueInDestructor {
  Child(bool* ptr) : SetTrueInDestructor(ptr) {}
};

ZC_TEST("Rc inheritance") {
  bool b = false;

  auto child = zc::rc<Child>(&b);

  // up casting works automatically
  zc::Rc<SetTrueInDestructor> parent = child.addRef();

  auto down = parent.downcast<Child>();
  EXPECT_TRUE(parent == nullptr);
  EXPECT_TRUE(down != nullptr);

  EXPECT_FALSE(b);
  child = nullptr;
  EXPECT_FALSE(b);
  down = nullptr;
  EXPECT_TRUE(b);
}

ZC_TEST("Refcounted::EnableAddRefToThis") {
  bool b = false;

  auto ref1 = zc::rc<SetTrueInDestructor>(&b);
  EXPECT_FALSE(ref1->isShared());

  auto ref2 = ref1->newRef();
  EXPECT_TRUE(ref2->isShared());
  EXPECT_TRUE(ref1->isShared());
  EXPECT_FALSE(b);

  ref1 = nullptr;
  EXPECT_FALSE(ref2->isShared());
  EXPECT_FALSE(b);

  ref2 = nullptr;
  EXPECT_TRUE(b);
}

struct SetTrueInDestructor2 {
  // Like above but doesn't inherit Refcounted.

  SetTrueInDestructor2(bool* ptr) : ptr(ptr) {}
  ~SetTrueInDestructor2() { *ptr = true; }

  bool* ptr;
};

ZC_TEST("RefcountedWrapper") {
  {
    bool b = false;
    Own<RefcountedWrapper<SetTrueInDestructor2>> w = refcountedWrapper<SetTrueInDestructor2>(&b);
    ZC_EXPECT(!b);

    Own<SetTrueInDestructor2> ref1 = w->addWrappedRef();
    Own<SetTrueInDestructor2> ref2 = w->addWrappedRef();

    ZC_EXPECT(ref1.get() == &w->getWrapped());
    ZC_EXPECT(ref1.get() == ref2.get());

    ZC_EXPECT(!b);

    w = nullptr;
    ref1 = nullptr;

    ZC_EXPECT(!b);

    ref2 = nullptr;

    ZC_EXPECT(b);
  }

  // Wrap Own<T>.
  {
    bool b = false;
    Own<RefcountedWrapper<Own<SetTrueInDestructor2>>> w =
        refcountedWrapper<SetTrueInDestructor2>(zc::heap<SetTrueInDestructor2>(&b));
    ZC_EXPECT(!b);

    Own<SetTrueInDestructor2> ref1 = w->addWrappedRef();
    Own<SetTrueInDestructor2> ref2 = w->addWrappedRef();

    ZC_EXPECT(ref1.get() == &w->getWrapped());
    ZC_EXPECT(ref1.get() == ref2.get());

    ZC_EXPECT(!b);

    w = nullptr;
    ref1 = nullptr;

    ZC_EXPECT(!b);

    ref2 = nullptr;

    ZC_EXPECT(b);
  }

  // Try wrapping an `int` to really demonstrate the wrapped type can be anything.
  {
    Own<RefcountedWrapper<int>> w = refcountedWrapper<int>(123);
    int* ptr = &w->getWrapped();
    ZC_EXPECT(*ptr == 123);

    Own<int> ref1 = w->addWrappedRef();
    Own<int> ref2 = w->addWrappedRef();

    ZC_EXPECT(ref1.get() == ptr);
    ZC_EXPECT(ref2.get() == ptr);

    w = nullptr;
    ref1 = nullptr;

    ZC_EXPECT(*ref2 == 123);
  }
}

struct AtomicSetTrueInDestructor : public AtomicRefcounted,
                                   EnableAddRefToThis<AtomicSetTrueInDestructor> {
  AtomicSetTrueInDestructor(bool* ptr) : ptr(ptr) {}
  ~AtomicSetTrueInDestructor() { *ptr = true; }

  zc::Arc<AtomicSetTrueInDestructor> newRef() { return addRefToThis(); }

  bool* ptr;
};

ZC_TEST("Arc") {
  bool b = false;

  zc::Arc<AtomicSetTrueInDestructor> ref1 = zc::arc<AtomicSetTrueInDestructor>(&b);
  EXPECT_FALSE(ref1->isShared());
  EXPECT_TRUE(ref1 != nullptr);
  EXPECT_FALSE(ref1 == nullptr);

  zc::Arc<AtomicSetTrueInDestructor> ref2 = ref1.addRef();

  // can be always cast to Arc<const T>
  zc::Arc<const AtomicSetTrueInDestructor> ref3 = ref1.addRef();

  // addRef works for const references too
  zc::Arc<const AtomicSetTrueInDestructor> ref4 = ref3.addRef();

  ref1 = nullptr;
  EXPECT_TRUE(ref1 == nullptr);
  ref2 = nullptr;
  EXPECT_TRUE(ref2 == nullptr);
  ref3 = nullptr;
  EXPECT_TRUE(ref3 == nullptr);

  EXPECT_FALSE(b);
  ref4 = nullptr;
  EXPECT_TRUE(b);
}

ZC_TEST("AtomicRefcounted::EnableAddRefToThis") {
  bool b = false;

  zc::Arc<AtomicSetTrueInDestructor> ref1 = zc::arc<AtomicSetTrueInDestructor>(&b);
  EXPECT_FALSE(ref1->isShared());

  zc::Arc<AtomicSetTrueInDestructor> ref2 = ref1->newRef();
  EXPECT_TRUE(ref2->isShared());
  EXPECT_TRUE(ref1->isShared());
  EXPECT_FALSE(b);

  ref1 = nullptr;
  EXPECT_FALSE(ref2->isShared());
  EXPECT_FALSE(b);

  ref2 = nullptr;
  EXPECT_TRUE(b);
}

ZC_TEST("Arc Own interop") {
  bool b = false;

  zc::Arc<AtomicSetTrueInDestructor> ref1 = zc::arc<AtomicSetTrueInDestructor>(&b);

  EXPECT_FALSE(b);
  auto own = ref1.toOwn();
  EXPECT_TRUE(ref1 == nullptr);
  EXPECT_TRUE(own.get() != nullptr);

  EXPECT_FALSE(b);
  own = nullptr;
  EXPECT_TRUE(b);
}

}  // namespace zc
