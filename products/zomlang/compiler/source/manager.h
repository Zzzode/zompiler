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

#pragma once

#include "zc/core/common.h"
#include "zc/core/filesystem.h"
#include "zc/core/memory.h"
#include "zc/core/string.h"
#include "zomlang/compiler/diagnostics/diagnostic.h"

namespace zomlang {
namespace compiler {
namespace source {

class SourceLoc;
class SourceRange;

struct LineAndColumn;

class SourceManager {
public:
  SourceManager() noexcept;
  ~SourceManager() noexcept(false);

  ZC_DISALLOW_COPY_AND_MOVE(SourceManager);

  // Buffer management
  uint64_t addNewSourceBuffer(zc::Array<zc::byte> inputData, zc::StringPtr bufIdentifier);
  uint64_t addMemBufferCopy(zc::ArrayPtr<const zc::byte> inputData, zc::StringPtr bufIdentifier);

  // Virtual file management
  void createVirtualFile(const SourceLoc& loc, zc::StringPtr name, int lineOffset, unsigned length);
  const struct VirtualFile* getVirtualFile(const SourceLoc& loc) const;

  // Generated source info
  void setGeneratedSourceInfo(uint64_t bufferId, const struct GeneratedSourceInfo& info);
  const struct GeneratedSourceInfo* getGeneratedSourceInfo(uint64_t bufferId) const;

  /// Returns the offset in bytes for the given valid source location.
  unsigned getLocOffsetInBuffer(SourceLoc Loc, uint64_t bufferId) const;

  /// Location and range operations
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
  void recordRegexLiteralStartLoc(const SourceLoc& loc);
  bool isRegexLiteralStart(const SourceLoc& loc) const;

private:
  class Impl;
  zc::Own<Impl> impl;
};

}  // namespace source
}  // namespace compiler
}  // namespace zomlang
