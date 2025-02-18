// Copyright (c) 2024-2025 Zode.Z. All rights reserved
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
#include "zc/core/memory.h"
#include "zc/core/string.h"
#include "zc/core/vector.h"
#include "zomlang/compiler/source/location.h"

namespace zomlang {
namespace compiler {

namespace source {
class CharSourceRange;
class SourceLoc;
}  // namespace source

namespace diagnostics {

enum class DiagnosticKind : uint8_t { kNote, kRemark, kWarning, kError, kFatal };

struct FixIt {
  source::CharSourceRange range;
  zc::String replacementText;
};

class Diagnostic {
public:
  Diagnostic(DiagnosticKind kind, uint32_t id, zc::StringPtr message,
             const source::CharSourceRange& location);
  ~Diagnostic();

  Diagnostic(Diagnostic&& other) noexcept;
  Diagnostic& operator=(Diagnostic&& other) noexcept;

  ZC_DISALLOW_COPY(Diagnostic);

  ZC_NODISCARD DiagnosticKind getKind() const;
  ZC_NODISCARD uint32_t getId() const;
  ZC_NODISCARD zc::StringPtr getMessage() const;
  ZC_NODISCARD const source::CharSourceRange& getSourceRange() const;
  ZC_NODISCARD const zc::Vector<zc::Own<Diagnostic>>& getChildDiagnostics() const;
  ZC_NODISCARD const zc::Vector<zc::Own<FixIt>>& getFixIts() const;

  void addChildDiagnostic(zc::Own<Diagnostic> child);
  void addFixIt(zc::Own<FixIt> fixIt);
  void setCategory(zc::StringPtr newCategory);

private:
  struct Impl;
  zc::Own<Impl> impl;
};

class DiagnosticConsumer {
public:
  virtual ~DiagnosticConsumer();
  virtual void handleDiagnostic(const source::SourceLoc& loc, const Diagnostic& diagnostic) = 0;

protected:
  struct Impl;
  zc::Own<Impl> impl;
};

}  // namespace diagnostics
}  // namespace compiler
}  // namespace zomlang