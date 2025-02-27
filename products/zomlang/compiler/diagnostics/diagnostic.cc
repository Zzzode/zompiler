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

#include "zc/core/debug.h"
#include "zc/core/vector.h"
#include "zomlang/compiler/diagnostics/diagnostic-engine.h"
#include "zomlang/compiler/diagnostics/diagnostic-ids.h"
#include "zomlang/compiler/diagnostics/diagnostic-info.h"
#include "zomlang/compiler/lexer/token.h"
#include "zomlang/compiler/source/location.h"
#include "zomlang/compiler/source/manager.h"

namespace zomlang {
namespace compiler {
namespace diagnostics {

// ================================================================================
// Diagnostic::Impl

struct Diagnostic::Impl {
  friend class Diagnostic;

  using Arguments = zc::OneOf<zc::ConstString,    // Str
                              source::SourceLoc,  // Loc
                              lexer::Token,       // Token
                              zc::String          //
                              >;

  template <typename... Args>
  explicit Impl(const DiagID id, const source::SourceLoc loc, Args&&... args)
      : id(id), location(loc), args{zc::fwd<Args>(args)...} {}

  DiagID id;
  source::SourceLoc location;
  zc::Vector<Arguments> args;
  zc::Vector<zc::Own<Diagnostic>> childDiagnostics;
  zc::Vector<zc::Own<FixIt>> fixIts;
  zc::Vector<source::CharSourceRange> ranges;
};

// ================================================================================
// Diagnostic

template <typename... Args>
Diagnostic::Diagnostic(DiagID id, source::SourceLoc loc, Args&&... args)
    : impl(zc::heap<Impl>(id, loc, zc::fwd<Args>(args)...)) {}
Diagnostic::~Diagnostic() = default;

Diagnostic::Diagnostic(Diagnostic&&) noexcept = default;
Diagnostic& Diagnostic::operator=(Diagnostic&&) noexcept = default;

DiagID Diagnostic::getId() const { return impl->id; }

const zc::Vector<zc::Own<Diagnostic>>& Diagnostic::getChildDiagnostics() const {
  return impl->childDiagnostics;
}

const zc::Vector<zc::Own<FixIt>>& Diagnostic::getFixIts() const { return impl->fixIts; }
const source::SourceLoc& Diagnostic::getLoc() const { return impl->location; }

void Diagnostic::addChildDiagnostic(zc::Own<Diagnostic> child) {
  impl->childDiagnostics.add(zc::mv(child));
}

void Diagnostic::addFixIt(zc::Own<FixIt> fixIt) { impl->fixIts.add(zc::mv(fixIt)); }

// ================================================================================
// DiagnosticConsumer::Impl

struct DiagnosticConsumer::Impl {
  // Extensible consumer state retention
};

DiagnosticConsumer::~DiagnosticConsumer() = default;

}  // namespace diagnostics
}  // namespace compiler
}  // namespace zomlang