#include "src/compiler/source/location.h"

namespace compiler {
namespace source {

SourceLocation::SourceLocation(std::string file, int line, int column)
    : file(std::move(file)), line(line), column(column) {}

const std::string& SourceLocation::getFile() const { return file; }
int SourceLocation::getLine() const { return line; }
int SourceLocation::getColumn() const { return column; }

}  // namespace source
}  // namespace compiler