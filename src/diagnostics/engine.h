#ifndef COMPILER_DIAGNOSTIC_ENGINE_H
#define COMPILER_DIAGNOSTIC_ENGINE_H

#include <memory>
#include <vector>

#include "src/diagnostics/consumer.h"
#include "src/diagnostics/diagnostic.h"

namespace diagnostic {

class DiagnosticEngine {
 public:
  DiagnosticEngine();

  void setDiagnosticConsumer(std::unique_ptr<DiagnosticConsumer> consumer);
  void emit(Diagnostic diagnostic);
  bool hasErrors() const;

 private:
  std::unique_ptr<DiagnosticConsumer> consumer;
  std::vector<Diagnostic> diagnostics;
  bool errors;
};

}  // namespace diagnostic

#endif  // COMPILER_DIAGNOSTIC_ENGINE_H