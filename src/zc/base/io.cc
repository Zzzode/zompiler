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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if _WIN32
#include "win32-api-version.h"
#endif

#include <errno.h>

#include "src/zc/base/debug.h"
#include "src/zc/base/io.h"
#include "src/zc/base/miniposix.h"
#include "src/zc/containers/vector.h"

#if _WIN32
#include <windows.h>

#include "windows-sanity.h"
#else
#include <sys/uio.h>
#endif

namespace zc {

InputStream::~InputStream() noexcept(false) {}
OutputStream::~OutputStream() noexcept(false) {}
BufferedInputStream::~BufferedInputStream() noexcept(false) {}
BufferedOutputStream::~BufferedOutputStream() noexcept(false) {}

size_t InputStream::read(ArrayPtr<byte> buffer, size_t minBytes) {
  size_t n = tryRead(buffer, minBytes);
  ZC_REQUIRE(n >= minBytes, "Premature EOF") {
    // Pretend we read zeros from the input.
    buffer.slice(n).first(minBytes - n).fill(0);
    return minBytes;
  }
  return n;
}

void InputStream::skip(size_t bytes) {
  byte scratch[8192]{};
  while (bytes > 0) {
    size_t amount = zc::min(bytes, sizeof(scratch));
    read(arrayPtr(scratch).first(amount));
    bytes -= amount;
  }
}

namespace {

Array<byte> readAll(InputStream& input, uint64_t limit, bool nulTerminate) {
  Vector<Array<byte>> parts;
  constexpr size_t BLOCK_SIZE = 4096;

  for (;;) {
    ZC_REQUIRE(limit > 0, "Reached limit before EOF.");
    auto part = heapArray<byte>(zc::min(BLOCK_SIZE, limit));
    size_t n = input.tryRead(part, part.size());
    limit -= n;
    if (n < part.size()) {
      auto result = heapArray<byte>(parts.size() * BLOCK_SIZE + n + nulTerminate);
      byte* pos = result.begin();
      for (auto& p : parts) {
        memcpy(pos, p.begin(), BLOCK_SIZE);
        pos += BLOCK_SIZE;
      }
      memcpy(pos, part.begin(), n);
      pos += n;
      if (nulTerminate) *pos++ = '\0';
      ZC_ASSERT(pos == result.end());
      return result;
    } else {
      parts.add(zc::mv(part));
    }
  }
}

}  // namespace

String InputStream::readAllText(uint64_t limit) {
  return String(readAll(*this, limit, true).releaseAsChars());
}
Array<byte> InputStream::readAllBytes(uint64_t limit) { return readAll(*this, limit, false); }

void OutputStream::write(ArrayPtr<const ArrayPtr<const byte>> pieces) {
  for (auto piece : pieces) { write(piece); }
}

ArrayPtr<const byte> BufferedInputStream::getReadBuffer() {
  auto result = tryGetReadBuffer();
  ZC_REQUIRE(result.size() > 0, "Premature EOF");
  return result;
}

// =======================================================================================

BufferedInputStreamWrapper::BufferedInputStreamWrapper(InputStream& inner, ArrayPtr<byte> buffer)
    : inner(inner),
      ownedBuffer(buffer == nullptr ? heapArray<byte>(8192) : nullptr),
      buffer(buffer == nullptr ? ownedBuffer : buffer) {}

BufferedInputStreamWrapper::~BufferedInputStreamWrapper() noexcept(false) {}

ArrayPtr<const byte> BufferedInputStreamWrapper::tryGetReadBuffer() {
  if (bufferAvailable.size() == 0) {
    size_t n = inner.tryRead(buffer, 1);
    bufferAvailable = buffer.first(n);
  }

  return bufferAvailable;
}

size_t BufferedInputStreamWrapper::tryRead(ArrayPtr<byte> dst, size_t minBytes) {
  size_t maxBytes = dst.size();
  if (minBytes <= bufferAvailable.size()) {
    // Serve from current buffer.
    size_t n = zc::min(bufferAvailable.size(), maxBytes);
    memcpy(dst.begin(), bufferAvailable.begin(), n);
    bufferAvailable = bufferAvailable.slice(n);
    return n;
  } else {
    // Copy current available into destination.
    memcpy(dst.begin(), bufferAvailable.begin(), bufferAvailable.size());
    size_t fromFirstBuffer = bufferAvailable.size();
    dst = dst.slice(fromFirstBuffer);
    minBytes -= fromFirstBuffer;
    maxBytes -= fromFirstBuffer;

    if (maxBytes <= buffer.size()) {
      // Read the next buffer-full.
      size_t n = inner.tryRead(buffer, minBytes);
      size_t fromSecondBuffer = zc::min(n, maxBytes);
      memcpy(dst.begin(), buffer.begin(), fromSecondBuffer);
      bufferAvailable = buffer.slice(fromSecondBuffer, n);
      return fromFirstBuffer + fromSecondBuffer;
    } else {
      // Forward large read to the underlying stream.
      bufferAvailable = nullptr;
      return fromFirstBuffer + inner.tryRead(dst, minBytes);
    }
  }
}

void BufferedInputStreamWrapper::skip(size_t bytes) {
  if (bytes <= bufferAvailable.size()) {
    bufferAvailable = bufferAvailable.slice(bytes, bufferAvailable.size());
  } else {
    bytes -= bufferAvailable.size();
    if (bytes <= buffer.size()) {
      // Read the next buffer-full.
      size_t n = inner.read(buffer, bytes);
      bufferAvailable = buffer.slice(bytes, n);
    } else {
      // Forward large skip to the underlying stream.
      bufferAvailable = nullptr;
      inner.skip(bytes);
    }
  }
}

// -------------------------------------------------------------------

BufferedOutputStreamWrapper::BufferedOutputStreamWrapper(OutputStream& inner, ArrayPtr<byte> buffer)
    : inner(inner),
      ownedBuffer(buffer == nullptr ? heapArray<byte>(8192) : nullptr),
      buffer(buffer == nullptr ? ownedBuffer : buffer),
      bufferPos(this->buffer.begin()) {}

BufferedOutputStreamWrapper::~BufferedOutputStreamWrapper() noexcept(false) {
  unwindDetector.catchExceptionsIfUnwinding([&]() { flush(); });
}

void BufferedOutputStreamWrapper::flush() {
  if (bufferPos > buffer.begin()) {
    inner.write(buffer.slice(0, bufferPos - buffer.begin()));
    bufferPos = buffer.begin();
  }
}

ArrayPtr<byte> BufferedOutputStreamWrapper::getWriteBuffer() {
  return arrayPtr(bufferPos, buffer.end());
}

void BufferedOutputStreamWrapper::write(ArrayPtr<const byte> src) {
  auto size = src.size();
  if (src.begin() == bufferPos) {
    // Oh goody, the caller wrote directly into our buffer.
    bufferPos += size;
  } else {
    size_t available = buffer.end() - bufferPos;

    if (size <= available) {
      memcpy(bufferPos, src.begin(), size);
      bufferPos += size;
    } else if (size <= buffer.size()) {
      // Too much for this buffer, but not a full buffer's worth, so we'll go
      // ahead and copy.
      memcpy(bufferPos, src.begin(), available);
      inner.write(buffer);

      size -= available;
      src = src.slice(available);

      memcpy(buffer.begin(), src.begin(), size);
      bufferPos = buffer.begin() + size;
    } else {
      // Writing so much data that we might as well write directly to avoid a
      // copy.
      inner.write(buffer.slice(0, bufferPos - buffer.begin()));
      bufferPos = buffer.begin();
      inner.write(src.first(size));
    }
  }
}

// =======================================================================================

ArrayInputStream::ArrayInputStream(ArrayPtr<const byte> array) : array(array) {}
ArrayInputStream::~ArrayInputStream() noexcept(false) {}

ArrayPtr<const byte> ArrayInputStream::tryGetReadBuffer() { return array; }

size_t ArrayInputStream::tryRead(ArrayPtr<byte> dst, size_t minBytes) {
  size_t n = zc::min(dst.size(), array.size());
  memcpy(dst.begin(), array.begin(), n);
  array = array.slice(n);
  return n;
}

void ArrayInputStream::skip(size_t bytes) {
  ZC_REQUIRE(array.size() >= bytes, "ArrayInputStream ended prematurely.") {
    bytes = array.size();
    break;
  }
  array = array.slice(bytes, array.size());
}

// -------------------------------------------------------------------

ArrayOutputStream::ArrayOutputStream(ArrayPtr<byte> array) : array(array), fillPos(array.begin()) {}
ArrayOutputStream::~ArrayOutputStream() noexcept(false) {}

ArrayPtr<byte> ArrayOutputStream::getWriteBuffer() { return arrayPtr(fillPos, array.end()); }

void ArrayOutputStream::write(ArrayPtr<const byte> src) {
  auto size = src.size();
  if (src.begin() == fillPos && fillPos != array.end()) {
    // Oh goody, the caller wrote directly into our buffer.
    ZC_REQUIRE(size <= array.end() - fillPos, size, fillPos, array.end() - fillPos);
    fillPos += size;
  } else {
    ZC_REQUIRE(size <= (size_t)(array.end() - fillPos),
               "ArrayOutputStream's backing array was not large enough for the "
               "data written.");
    memcpy(fillPos, src.begin(), size);
    fillPos += size;
  }
}

// -------------------------------------------------------------------

VectorOutputStream::VectorOutputStream(size_t initialCapacity)
    : vector(heapArray<byte>(initialCapacity)), fillPos(vector.begin()) {}
VectorOutputStream::~VectorOutputStream() noexcept(false) {}

ArrayPtr<byte> VectorOutputStream::getWriteBuffer() {
  // Grow if needed.
  if (fillPos == vector.end()) { grow(vector.size() + 1); }

  return arrayPtr(fillPos, vector.end());
}

void VectorOutputStream::write(ArrayPtr<const byte> src) {
  auto size = src.size();
  size_t remaining = static_cast<size_t>(vector.end() - fillPos);

  if (src.begin() == fillPos && fillPos != vector.end()) {
    // Oh goody, the caller wrote directly into our buffer.
    ZC_REQUIRE(size <= remaining, size, fillPos, vector.end() - fillPos);
    fillPos += size;
  } else {
    if (remaining < size) { grow(fillPos - vector.begin() + size); }

    memcpy(fillPos, src.begin(), size);
    fillPos += size;
  }
}

void VectorOutputStream::grow(size_t minSize) {
  size_t newSize = vector.size() * 2;
  while (newSize < minSize) newSize *= 2;
  auto newVector = heapArray<byte>(newSize);
  memcpy(newVector.begin(), vector.begin(), fillPos - vector.begin());
  fillPos = fillPos - vector.begin() + newVector.begin();
  vector = zc::mv(newVector);
}

// =======================================================================================

AutoCloseFd::~AutoCloseFd() noexcept(false) {
  if (fd >= 0) {
    // Don't use SYSCALL() here because close() should not be repeated on EINTR.
    if (miniposix::close(fd) < 0) {
      ZC_FAIL_SYSCALL("close", errno, fd) {
        // This ensures we don't throw an exception if unwinding.
        break;
      }
    }
  }
}

FdInputStream::~FdInputStream() noexcept(false) {}

size_t FdInputStream::tryRead(ArrayPtr<byte> buffer, size_t minBytes) {
  byte* pos = buffer.begin();
  byte* min = pos + minBytes;
  byte* max = buffer.end();

  while (pos < min) {
    miniposix::ssize_t n;
    ZC_SYSCALL(n = miniposix::read(fd, pos, max - pos), fd);
    if (n == 0) { break; }
    pos += n;
  }

  return pos - buffer.begin();
}

FdOutputStream::~FdOutputStream() noexcept(false) {}

void FdOutputStream::write(ArrayPtr<const byte> data) {
  auto size = data.size();
  auto pos = data.begin();

  while (size > 0) {
    miniposix::ssize_t n;
    ZC_SYSCALL(n = miniposix::write(fd, pos, size), fd);
    ZC_ASSERT(n > 0, "write() returned zero.");
    pos += n;
    size -= n;
  }
}

void FdOutputStream::write(ArrayPtr<const ArrayPtr<const byte>> pieces) {
#if _WIN32
  // Windows has no reasonable writev(). It has WriteFileGather, but this call
  // has the unreasonable restriction that each segment must be page-aligned.
  // So, fall back to the default implementation
  for (auto piece : pieces) write(piece);
#else
  const size_t iovmax = miniposix::iovMax();
  while (pieces.size() > iovmax) {
    write(pieces.first(iovmax));
    pieces = pieces.slice(iovmax, pieces.size());
  }

  ZC_STACK_ARRAY(struct iovec, iov, pieces.size(), 16u, 128);

  for (uint i = 0; i < pieces.size(); i++) {
    // writev() interface is not const-correct. :(
    iov[i].iov_base = const_cast<byte*>(pieces[i].begin());
    iov[i].iov_len = pieces[i].size();
  }

  struct iovec* current = iov.begin();

  // Advance past any leading empty buffers so that a write full of only empty
  // buffers does not cause a syscall at all.
  while (current < iov.end() && current->iov_len == 0) { ++current; }

  while (current < iov.end()) {
    // Issue the write.
    ssize_t n = 0;
    ZC_SYSCALL(n = ::writev(fd, current, iov.end() - current), fd);
    ZC_ASSERT(n > 0, "writev() returned zero.");

    // Advance past all buffers that were fully-written.
    while (current < iov.end() && static_cast<size_t>(n) >= current->iov_len) {
      n -= current->iov_len;
      ++current;
    }

    // If we only partially-wrote one of the buffers, adjust the pointer and
    // size to include only the unwritten part.
    if (n > 0) {
      current->iov_base = reinterpret_cast<byte*>(current->iov_base) + n;
      current->iov_len -= n;
    }
  }
#endif
}

// =======================================================================================

#if _WIN32

AutoCloseHandle::~AutoCloseHandle() noexcept(false) {
  if (handle != (void*)-1) { ZC_WIN32(CloseHandle(handle)); }
}

HandleInputStream::~HandleInputStream() noexcept(false) {}

size_t HandleInputStream::tryRead(ArrayPtr<byte> buffer, size_t minBytes) {
  byte* pos = buffer.begin();
  byte* min = pos + minBytes;
  byte* max = buffer.end();

  while (pos < min) {
    DWORD n;
    ZC_WIN32(ReadFile(handle, pos, zc::min(max - pos, DWORD(zc::maxValue)), &n, nullptr));
    if (n == 0) { break; }
    pos += n;
  }

  return pos - buffer.begin();
}

HandleOutputStream::~HandleOutputStream() noexcept(false) {}

void HandleOutputStream::write(ArrayPtr<const byte> buffer) {
  const char* pos = buffer.asChars().begin();
  size_t size = buffer.size();

  while (size > 0) {
    DWORD n;
    ZC_WIN32(WriteFile(handle, pos, zc::min(size, DWORD(zc::maxValue)), &n, nullptr));
    ZC_ASSERT(n > 0, "write() returned zero.");
    pos += n;
    size -= n;
  }
}

#endif  // _WIN32

}  // namespace zc
