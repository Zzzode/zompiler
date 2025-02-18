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

#include "zomlang/compiler/driver/driver.h"

#include "zc/core/filesystem.h"
#include "zomlang/compiler/basic/frontend.h"
#include "zomlang/compiler/diagnostics/diagnostic-engine.h"
#include "zomlang/compiler/source/manager.h"
#include "zomlang/compiler/source/module.h"

namespace zomlang {
namespace compiler {
namespace driver {

// ================================================================================
// CompilerDriver::Impl

class CompilerDriver::Impl {
public:
  Impl() noexcept;
  ~Impl() noexcept(false);

  ZC_DISALLOW_COPY_AND_MOVE(Impl);

  struct OutputDirective {
    zc::ArrayPtr<zc::byte> name;
    zc::Maybe<zc::Path> dir;

    ZC_DISALLOW_COPY(OutputDirective);
    OutputDirective(OutputDirective&&) noexcept = default;
    OutputDirective(const zc::ArrayPtr<zc::byte> name, zc::Maybe<zc::Path> dir)
        : name(name), dir(zc::mv(dir)) {}
  };

  zc::Maybe<const source::Module&> addSourceFileImpl(zc::StringPtr file);

private:
  /// Module loader to transform sources to a module representation.
  zc::Own<source::ModuleLoader> loader;
  /// Source manager to manage source files.
  zc::Own<source::SourceManager> sourceManager;
  /// Diagnostic engine to report diagnostics.
  zc::Own<diagnostics::DiagnosticEngine> diagnosticEngine;
};

CompilerDriver::Impl::Impl() noexcept
    : loader(zc::heap<source::ModuleLoader>()),
      sourceManager(zc::heap<source::SourceManager>()),
      diagnosticEngine(zc::heap<diagnostics::DiagnosticEngine>(*sourceManager)) {}

CompilerDriver::Impl::~Impl() noexcept(false) = default;

zc::Maybe<const source::Module&> CompilerDriver::Impl::addSourceFileImpl(const zc::StringPtr file) {
  uint64_t bufferId = sourceManager->getExternalSourceBufferID(file);
  const zc::StringPtr moduleName = sourceManager->getIdentifierForBuffer(bufferId);
  return loader->loadModule(moduleName, bufferId);
}

// ================================================================================
// CompilerDriver

CompilerDriver::CompilerDriver() noexcept : impl(zc::heap<Impl>()) {}
CompilerDriver::~CompilerDriver() noexcept(false) = default;

zc::Maybe<const source::Module&> CompilerDriver::addSourceFile(const zc::StringPtr file) {
  return impl->addSourceFileImpl(file);
}

}  // namespace driver
}  // namespace compiler
}  // namespace zomlang
