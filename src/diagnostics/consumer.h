#ifndef COMPILER_DIAGNOSTIC_CONSUMER_H
#define COMPILER_DIAGNOSTIC_CONSUMER_H

#include <iostream>

#include "src/diagnostics/diagnostic.h"

namespace diagnostic {

class DiagnosticConsumer {
 public:
  virtual ~DiagnosticConsumer() = default;
  virtual void consume(const Diagnostic& diagnostic) = 0;
};

class StreamDiagnosticConsumer : public DiagnosticConsumer {
 public:
  explicit StreamDiagnosticConsumer(std::ostream& out = std::cerr);
  void consume(const Diagnostic& diagnostic) override;

 private:
  std::ostream& out;
};

}  // namespace diagnostic

#endif