#include <fstream>
#include <iostream>
#include <sstream>

#include "src/bnf_calculator/calculator.h"
#include "src/bnf_calculator/lexer.h"
#include "src/bnf_calculator/parser.h"
#include "src/compiler/diagnostic/engine.h"
#include "src/compiler/source/location.h"
#include "src/zc/base/hints.h"
#include "src/zc/utility/one_of.h"

using namespace bnf_calculator;

void printSet(const std::string& setName, const Calculator::SetMap& set) {
  for (const auto& [symbol, symbolSet] : set) {
    std::cout << setName << "(" << symbol << ") = { ";
    for (const auto& item : symbolSet) {
      std::cout << item << " ";
    }
    std::cout << "}\n";
  }
  std::cout << std::endl;
}

int main(int argc, char* argv[]) {
  compiler::diagnostic::DiagnosticEngine diagnosticEngine;

  if (argc != 2) {
    diagnosticEngine.emit(compiler::diagnostic::Diagnostic(
        compiler::diagnostic::DiagnosticSeverity::Error,
        "Usage: " + std::string(argv[0]) + " <input_file>",
        compiler::source::SourceLocation("", 0, 0)));  // Default location
    return 1;
  }

  std::string inputFilePath = argv[1];
  std::ifstream inputFile(inputFilePath);
  if (!inputFile) ZC_UNLIKELY {
      diagnosticEngine.emit(compiler::diagnostic::Diagnostic(
          compiler::diagnostic::DiagnosticSeverity::Error,
          "Unable to open input file.",
          compiler::source::SourceLocation(inputFilePath, 0,
                                           0)));  // Use 0 for line and column
      return 1;
    }

  std::stringstream buffer;
  buffer << inputFile.rdbuf();
  std::string input = buffer.str();

  try {
    Lexer lexer(input, diagnosticEngine);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    auto grammar = parser.parse();

    Calculator calculator(*grammar);

    auto firstSets = calculator.computeFirstSets();
    std::cout << "FIRST Sets:\n";
    printSet("FIRST", firstSets);

    auto followSets = calculator.computeFollowSets();
    std::cout << "FOLLOW Sets:\n";
    printSet("FOLLOW", followSets);

    auto selectSets = calculator.computeSelectSets();
    std::cout << "SELECT Sets:\n";
    for (const auto& [production, set] : selectSets) {
      std::cout << "Production: " << production << std::endl;
      printSet("SELECT", set);
    }

  } catch (const std::exception& e) {
    diagnosticEngine.emit(compiler::diagnostic::Diagnostic(
        compiler::diagnostic::DiagnosticSeverity::Error, e.what(),
        compiler::source::SourceLocation(inputFilePath, 0, 0)));
  }

  // Check for errors and report them
  if (diagnosticEngine.hasErrors()) {
    return 1;
  }

  zc::OneOf<int, std::string> a;

  return 0;
}