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

#include "zomlang/compiler/diagnostics/diagnostic.h"

#include "zc/core/string.h"
#include "zc/core/vector.h"
#include "zomlang/compiler/source/location.h"

namespace zomlang {
namespace compiler {
namespace diagnostics {

// ================================================================================
// Diagnostic::Impl

struct Diagnostic::Impl {
  Impl(DiagnosticKind kind, uint32_t id, zc::String message,
       const source::CharSourceRange& location)
      : kind(kind), id(id), message(zc::mv(message)), location(location) {}

  DiagnosticKind kind;
  uint32_t id;
  zc::String message;
  source::CharSourceRange location;
  zc::String category;
  zc::Vector<zc::Own<Diagnostic>> childDiagnostics;
  zc::Vector<zc::Own<FixIt>> fixIts;
};

// ================================================================================
// Diagnostic

Diagnostic::Diagnostic(DiagnosticKind kind, uint32_t id, zc::StringPtr message,
                       const source::CharSourceRange& location)
    : impl(zc::heap<Impl>(kind, id, zc::heapString(message), location)) {}

Diagnostic::~Diagnostic() = default;

Diagnostic::Diagnostic(Diagnostic&&) noexcept = default;
Diagnostic& Diagnostic::operator=(Diagnostic&&) noexcept = default;

DiagnosticKind Diagnostic::getKind() const { return impl->kind; }
uint32_t Diagnostic::getId() const { return impl->id; }
zc::StringPtr Diagnostic::getMessage() const { return impl->message; }
const source::CharSourceRange& Diagnostic::getSourceRange() const { return impl->location; }
const zc::Vector<zc::Own<Diagnostic>>& Diagnostic::getChildDiagnostics() const {
  return impl->childDiagnostics;
}
const zc::Vector<zc::Own<FixIt>>& Diagnostic::getFixIts() const { return impl->fixIts; }

void Diagnostic::addChildDiagnostic(zc::Own<Diagnostic> child) {
  impl->childDiagnostics.add(zc::mv(child));
}

void Diagnostic::addFixIt(zc::Own<FixIt> fixIt) { impl->fixIts.add(zc::mv(fixIt)); }

void Diagnostic::setCategory(zc::StringPtr newCategory) {
  impl->category = zc::heapString(newCategory);
}

// ================================================================================
// DiagnosticConsumer::Impl

struct DiagnosticConsumer::Impl {
  // Extensible consumer state retention
};

DiagnosticConsumer::~DiagnosticConsumer() = default;

}  // namespace diagnostics
}  // namespace compiler
}  // namespace zomlang