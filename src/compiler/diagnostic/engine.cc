#include "src/compiler/diagnostic/engine.h"

namespace compiler {
namespace diagnostic {

DiagnosticEngine::DiagnosticEngine() : errors(false) {
  setDiagnosticConsumer(std::make_unique<StreamDiagnosticConsumer>());
}

void DiagnosticEngine::setDiagnosticConsumer(
    std::unique_ptr<DiagnosticConsumer> newDiagnosticConsumer) {
  consumer = std::move(newDiagnosticConsumer);
}

void DiagnosticEngine::emit(Diagnostic diagnostic) {
  if (diagnostic.getSeverity() >= DiagnosticSeverity::Error) {
    errors = true;
  }
  diagnostics.push_back(diagnostic);
  consumer->consume(diagnostic);
}

bool DiagnosticEngine::hasErrors() const { return errors; }

}  // namespace diagnostic
}  // namespace compiler