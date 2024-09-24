#include "src/diagnostics/consumer.h"

namespace diagnostic {

StreamDiagnosticConsumer::StreamDiagnosticConsumer(std::ostream& out)
    : out(out) {}

void StreamDiagnosticConsumer::consume(const Diagnostic& diagnostic) {
  out << diagnostic.getSourceLocation().getFile() << ":"
      << diagnostic.getSourceLocation().getLine() << ":"
      << diagnostic.getSourceLocation().getColumn() << ": ";

  switch (diagnostic.getSeverity()) {
    case DiagnosticSeverity::Note:
      out << "note: ";
      break;
    case DiagnosticSeverity::Warning:
      out << "warning: ";
      break;
    case DiagnosticSeverity::Error:
      out << "error: ";
      break;
    case DiagnosticSeverity::Fatal:
      out << "fatal error: ";
      break;
  }

  out << diagnostic.getMessage() << "\n";

  for (const auto& note : diagnostic.getNotes()) {
    out << "note: " << note << "\n";
  }
}

}  // namespace diagnostic
