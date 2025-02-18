// Copyright (c) 2025 Zode.Z. All rights reserved
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

#include "zomlang/compiler/source/manager.h"

#include <zc/core/map.h>

#include "zc/core/debug.h"
#include "zc/core/filesystem.h"
#include "zc/core/string.h"
#include "zomlang/compiler/source/location.h"

namespace zomlang {
namespace compiler {
namespace source {

struct LineAndColumn {
  unsigned line;
  unsigned column;
  LineAndColumn(const unsigned l, const unsigned c) : line(l), column(c) {}
};

// ================================================================================
// SourceManager::Impl

class SourceManager::Impl {
public:
  struct VirtualFile {
    CharSourceRange range;
    zc::StringPtr name;
    int lineOffset;
  };

  struct GeneratedSourceInfo {
    zc::String originalSource;
    zc::String generatedSource;
    zc::Array<diagnostics::FixIt> fixIts;
  };

  struct Buffer {
    const uint64_t id;
    zc::String identifier;
    zc::Array<zc::byte> data;
    /// The original source location of this buffer.
    GeneratedSourceInfo generatedInfo;
    /// The virtual files associated with this buffer.
    zc::Vector<VirtualFile> virtualFiles;
    /// The offset in bytes of the first character in the buffer.
    mutable zc::Vector<unsigned> lineStartOffsets;

    Buffer(const uint64_t id, zc::String identifier, zc::Array<zc::byte> data)
        : id(id), identifier(zc::mv(identifier)), data(zc::mv(data)) {}
  };

  Impl() noexcept;
  ~Impl() noexcept(false);

  // Buffer management
  uint64_t addNewSourceBuffer(zc::Array<zc::byte> inputData, zc::StringPtr bufIdentifier);
  uint64_t addMemBufferCopy(zc::ArrayPtr<const zc::byte> inputData, zc::StringPtr bufIdentifier);

  // Virtual file management
  void createVirtualFile(const SourceLoc& loc, zc::StringPtr name, int lineOffset, unsigned length);
  const VirtualFile* getVirtualFile(const SourceLoc& loc) const;

  // Generated source info
  void setGeneratedSourceInfo(uint64_t bufferId, const GeneratedSourceInfo& info);
  const GeneratedSourceInfo* getGeneratedSourceInfo(uint64_t bufferId) const;

  /// Returns the offset in bytes for the given valid source location.
  unsigned getLocOffsetInBuffer(SourceLoc loc, uint64_t bufferID) const;

  // Location and range operations
  SourceLoc getLocForOffset(uint64_t bufferId, unsigned offset) const;
  LineAndColumn getLineAndColumn(const SourceLoc& loc) const;
  unsigned getLineNumber(const SourceLoc& loc) const;
  bool isBefore(const SourceLoc& first, const SourceLoc& second) const;
  bool isAtOrBefore(const SourceLoc& first, const SourceLoc& second) const;
  bool containsTokenLoc(const SourceRange& range, const SourceLoc& loc) const;
  bool encloses(const SourceRange& enclosing, const SourceRange& inner) const;

  // Content retrieval
  zc::ArrayPtr<const zc::byte> getEntireTextForBuffer(uint64_t bufferId) const;
  zc::ArrayPtr<const zc::byte> extractText(const SourceRange& range) const;

  // Buffer identification
  uint64_t findBufferContainingLoc(const SourceLoc& loc) const;
  zc::StringPtr getFilename(uint64_t bufferId) const;

  // Line and column operations
  zc::Maybe<unsigned> resolveFromLineCol(uint64_t bufferId, unsigned line, unsigned col) const;
  zc::Maybe<unsigned> resolveOffsetForEndOfLine(uint64_t bufferId, unsigned line) const;
  zc::Maybe<unsigned> getLineLength(uint64_t bufferId, unsigned line) const;
  SourceLoc getLocForLineCol(uint64_t bufferId, unsigned line, unsigned col) const;

  // External source support
  uint64_t getExternalSourceBufferID(zc::StringPtr path);
  SourceLoc getLocFromExternalSource(zc::StringPtr path, unsigned line, unsigned col);

  // Diagnostics
  void getMessage(const SourceLoc& loc, diagnostics::DiagnosticKind kind, zc::StringPtr msg,
                  zc::ArrayPtr<SourceRange> ranges, zc::ArrayPtr<diagnostics::FixIt> fixIts,
                  zc::OutputStream& os) const;

  // Verification
  void verifyAllBuffers() const;

  // Regex literal support
  void recordRegexLiteralStartLoc(const SourceLoc loc);
  bool isRegexLiteralStart(const SourceLoc& loc) const;

private:
  /// The filesystem to use for reading files.
  zc::Own<const zc::Filesystem> fs;
  /// File a path to BufferID mapping cache
  zc::HashMap<zc::String, uint64_t> pathToBufferId;
  /// Whether to open in volatile mode (disallow memory mappings)
  bool openAsVolatile = false;

  zc::Vector<VirtualFile> virtualFiles;
  zc::Vector<SourceLoc> regexLiteralStartLocs;

  zc::Vector<zc::Own<Buffer>> buffers;
  /// Fast lookup from buffer ID to buffer.
  zc::HashMap<uint64_t, Buffer&> idToBuffer;

  mutable struct BufferLocCache {
    zc::Vector<uint64_t> sortedBuffers;
    uint64_t numBuffersOriginal = 0;
    zc::Maybe<uint64_t> lastBufferId;
  } locCache;

  void updateLocCache() const;
};

// ================================================================================
// SourceManager::Impl

SourceManager::Impl::Impl() noexcept : fs(zc::newDiskFilesystem()) {}
SourceManager::Impl::~Impl() noexcept(false) = default;

uint64_t SourceManager::Impl::addNewSourceBuffer(zc::Array<zc::byte> inputData,
                                                 const zc::StringPtr bufIdentifier) {
  auto buffer =
      zc::heap<Buffer>(buffers.size() + 1, zc::heapString(bufIdentifier), zc::mv(inputData));
  buffers.add(zc::mv(buffer));
  idToBuffer.insert(buffer->id, *buffers.back());
  return buffer->id;
}

uint64_t SourceManager::Impl::addMemBufferCopy(const zc::ArrayPtr<const zc::byte> inputData,
                                               const zc::StringPtr bufIdentifier) {
  auto buffer =
      zc::heap<Buffer>(buffers.size() + 1, zc::heapString(bufIdentifier), zc::heapArray(inputData));
  buffers.add(zc::mv(buffer));
  idToBuffer.insert(buffer->id, *buffers.back());
  return buffer->id;
}

void SourceManager::Impl::createVirtualFile(const SourceLoc& loc, zc::StringPtr name,
                                            int lineOffset, unsigned length) {
  VirtualFile vf;
  vf.range = CharSourceRange{loc, length};
  vf.name = name;
  vf.lineOffset = lineOffset;
  virtualFiles.add(zc::mv(vf));
}

unsigned SourceManager::Impl::getLocOffsetInBuffer(SourceLoc loc, uint64_t bufferID) const {
  ZC_ASSERT(loc.isValid(), "invalid loc");
  auto buffer = ... ZC_ASSERT(...., "Location is not from the specified buffer");
  return loc.value.getPointer() - buffer.begin();
}

zc::ArrayPtr<const zc::byte> SourceManager::Impl::getEntireTextForBuffer(uint64_t bufferId) const {}

zc::Maybe<unsigned> SourceManager::Impl::resolveFromLineCol(uint64_t bufferId, unsigned line,
                                                            unsigned col) const {
  const zc::ArrayPtr<const zc::byte> buffer = getEntireTextForBuffer(bufferId);

  unsigned currentLine = 1;
  unsigned currentCol = 1;
  for (size_t offset = 0; offset < buffer.size(); ++offset) {
    if (currentLine == line && currentCol == col) { return offset; }

    if (const char ch = static_cast<char>(buffer[offset]); ch == '\n') {
      ++currentLine;
      currentCol = 1;
    } else {
      ++currentCol;
    }
  }

  return zc::none;
}

uint64_t SourceManager::Impl::getExternalSourceBufferID(const zc::StringPtr path) {
  ZC_IF_SOME(bufferId, pathToBufferId.find(path)) { return bufferId; }

  const zc::PathPtr cwd = fs->getCurrentPath();
  zc::Path nativePath = cwd.evalNative(path);
  ZC_REQUIRE(path.size() > 0);

  const zc::ReadableDirectory& dir = nativePath.startsWith(cwd) ? fs->getCurrent() : fs->getRoot();
  const zc::Path sourcePath = nativePath.startsWith(cwd)
                                  ? nativePath.slice(cwd.size(), nativePath.size()).clone()
                                  : zc::mv(nativePath);

  ZC_IF_SOME(file, dir.tryOpenFile(sourcePath)) {
    zc::Array<zc::byte> data = file->readAllBytes();
    const uint64_t bufferId = addNewSourceBuffer(zc::mv(data), sourcePath.toString());
    pathToBufferId.insert(sourcePath.toString(), bufferId);
  }

  ZC_FAIL_ASSERT("Cannot open file path at ", path, ", no such file or directory.");
  ZC_KNOWN_UNREACHABLE();
}

SourceLoc SourceManager::Impl::getLocFromExternalSource(zc::StringPtr path, unsigned line,
                                                        unsigned col) {
  const uint64_t bufferId = getExternalSourceBufferID(path);
  if (bufferId == 0) return {};

  ZC_IF_SOME(offset, resolveFromLineCol(bufferId, line, col)) {
    return getLocForOffset(bufferId, offset);
  }

  return {};
}

void SourceManager::Impl::getMessage(const SourceLoc& loc, diagnostics::DiagnosticKind kind,
                                     const zc::StringPtr msg, zc::ArrayPtr<SourceRange> ranges,
                                     zc::ArrayPtr<diagnostics::FixIt> fixIts,
                                     zc::OutputStream& os) const {}

// ================================================================================
// SourceManager

SourceManager::SourceManager() noexcept : impl(zc::heap<Impl>()) {}
SourceManager::~SourceManager() noexcept(false) = default;

uint64_t SourceManager::getExternalSourceBufferID(const zc::StringPtr path) {
  return impl->getExternalSourceBufferID(path);
}

SourceLoc SourceManager::getLocFromExternalSource(zc::StringPtr path, unsigned line, unsigned col) {
  return impl->getLocFromExternalSource(path, line, col);
}

uint64_t SourceManager::addNewSourceBuffer(zc::Array<zc::byte> inputData,
                                           const zc::StringPtr bufIdentifier) {
  return impl->addNewSourceBuffer(zc::mv(inputData), bufIdentifier);
}

uint64_t SourceManager::addMemBufferCopy(const zc::ArrayPtr<const zc::byte> inputData,
                                         const zc::StringPtr bufIdentifier) {
  return impl->addMemBufferCopy(inputData, bufIdentifier);
}

void SourceManager::createVirtualFile(const SourceLoc& loc, const zc::StringPtr name,
                                      const int lineOffset, const unsigned length) {
  impl->createVirtualFile(loc, name, lineOffset, length);
}

unsigned SourceManager::getLocOffsetInBuffer(SourceLoc Loc, uint64_t bufferId) const {
  return impl->getLocOffsetInBuffer(Loc, bufferId);
}

void SourceManager::getMessage(const SourceLoc& loc, diagnostics::DiagnosticKind kind,
                               const zc::StringPtr msg, zc::ArrayPtr<SourceRange> ranges,
                               zc::ArrayPtr<diagnostics::FixIt> fixIts,
                               zc::OutputStream& os) const {
  impl->getMessage(loc, kind, msg, ranges, fixIts, os);
}

}  // namespace source
}  // namespace compiler
}  // namespace zomlang
