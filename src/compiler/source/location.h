#ifndef COMPILER_SOURCE_SOURCE_LOCATION_H
#define COMPILER_SOURCE_SOURCE_LOCATION_H

#include <string>

namespace compiler {
namespace source {

class SourceLocation {
 public:
  SourceLocation(std::string file, int line, int column);

  const std::string& getFile() const;
  int getLine() const;
  int getColumn() const;

 private:
  std::string file;
  int line;
  int column;
};

}  // namespace source
}  // namespace compiler

#endif  // COMPILER_SOURCE_SOURCE_LOCATION_H