#include "src/diagnostics/diagnostic.h"

namespace diagnostic {

Diagnostic::Diagnostic(DiagnosticSeverity severity, std::string message,
                       compiler::source::SourceLocation location)
    : severity(severity),
      message(std::move(message)),
      location(std::move(location)) {}

DiagnosticSeverity Diagnostic::getSeverity() const { return severity; }
const std::string& Diagnostic::getMessage() const { return message; }
const compiler::source::SourceLocation& Diagnostic::getSourceLocation() const {
  return location;
}
const std::vector<std::string>& Diagnostic::getNotes() const { return notes; }

void Diagnostic::addNote(std::string note) { notes.push_back(std::move(note)); }

}  // namespace diagnostic