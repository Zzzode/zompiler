#pragma once

#include "zc/core/common.h"
#include "zomlang/compiler/diagnostics/diagnostic-engine.h"
#include "zomlang/compiler/diagnostics/diagnostic.h"
#include "zomlang/compiler/source/location.h"

namespace zomlang {
namespace compiler {
namespace diagnostics {

class InFlightDiagnostic {
public:
  InFlightDiagnostic(DiagnosticEngine& engine, source::SourceLoc loc, Diagnostic&& diag)
      : engine(engine), loc(loc), diag(zc::mv(diag)), emitted(false) {}

  InFlightDiagnostic(InFlightDiagnostic&& other) noexcept = default;
  InFlightDiagnostic& operator=(InFlightDiagnostic&& other) noexcept = delete;

  ZC_DISALLOW_COPY(InFlightDiagnostic);

  ~InFlightDiagnostic() {
    if (!emitted) { emit(); }
  }

  void emit() {
    if (!emitted) {
      engine.emit(loc, zc::mv(diag));
      emitted = true;
    }
  }

  // Add methods to modify the diagnostic, for example, add fix-its
  InFlightDiagnostic& addFixIt(zc::Own<FixIt> fixit) {
    diag.addFixIt(zc::mv(fixit));
    return *this;
  }

private:
  DiagnosticEngine& engine;
  source::SourceLoc loc;
  Diagnostic diag;
  bool emitted;
};

}  // namespace diagnostics
}  // namespace compiler
}  // namespace zomlang