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

#include "zc/core/memory.h"

namespace zomlang {
namespace compiler {

namespace source {
class SourceManager;
class SourceLoc;
}  // namespace source

namespace diagnostics {

class Diagnostic;
class DiagnosticConsumer;
class DiagnosticState;

class InFlightDiagnostic;

class DiagnosticEngine {
public:
  explicit DiagnosticEngine(source::SourceManager& sourceManager);
  ~DiagnosticEngine();

  ZC_DISALLOW_COPY_AND_MOVE(DiagnosticEngine);

  void addConsumer(zc::Own<DiagnosticConsumer> consumer);
  void emit(const source::SourceLoc& loc, const Diagnostic& diagnostic);
  ZC_NODISCARD bool hasErrors() const;
  ZC_NODISCARD source::SourceManager& getSourceManager() const;
  ZC_NODISCARD const DiagnosticState& getState() const;

  DiagnosticState& getState();
  InFlightDiagnostic diagnose(const source::SourceLoc& loc);

private:
  struct Impl;
  zc::Own<Impl> impl;
};

}  // namespace diagnostics
}  // namespace compiler
}  // namespace zomlang
