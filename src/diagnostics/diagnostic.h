#ifndef COMPILER_DIAGNOSTIC_DIAGNOSTIC_H_
#define COMPILER_DIAGNOSTIC_DIAGNOSTIC_H_

#include <string>
#include <vector>

#include "src/compiler/source/location.h"

namespace diagnostic {

enum class DiagnosticSeverity { Note, Warning, Error, Fatal };

class Diagnostic {
 public:
  Diagnostic(DiagnosticSeverity severity, std::string message,
             compiler::source::SourceLocation location);

  DiagnosticSeverity getSeverity() const;
  const std::string& getMessage() const;
  const compiler::source::SourceLocation& getSourceLocation() const;
  const std::vector<std::string>& getNotes() const;

  void addNote(std::string note);

 private:
  DiagnosticSeverity severity;
  std::string message;
  compiler::source::SourceLocation location;
  std::vector<std::string> notes;
};

}  // namespace diagnostic

#endif