#include "src/compiler/diagnostic/diagnostic.h"

namespace compiler {
namespace diagnostic {

Diagnostic::Diagnostic(DiagnosticSeverity severity, std::string message,
                       source::SourceLocation location)
    : severity(severity),
      message(std::move(message)),
      location(std::move(location)) {}

DiagnosticSeverity Diagnostic::getSeverity() const { return severity; }
const std::string& Diagnostic::getMessage() const { return message; }
const source::SourceLocation& Diagnostic::getSourceLocation() const {
  return location;
}
const std::vector<std::string>& Diagnostic::getNotes() const { return notes; }

void Diagnostic::addNote(std::string note) { notes.push_back(std::move(note)); }

}  // namespace diagnostic
}  // namespace compiler