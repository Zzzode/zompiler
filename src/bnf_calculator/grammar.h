#ifndef BNF_CALCULATOR_GRAMMAR_H
#define BNF_CALCULATOR_GRAMMAR_H

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace bnf_calculator {

class Symbol {
 public:
  std::string name;
  bool isTerminal;

  Symbol(std::string name, bool isTerminal)
      : name(std::move(name)), isTerminal(isTerminal) {}
};

class Production {
 public:
  std::unique_ptr<Symbol> lhs;
  std::vector<std::vector<std::unique_ptr<Symbol>>> rhs;
};

class Grammar {
 public:
  std::vector<std::unique_ptr<Production>> rules;
  std::unordered_set<std::string> nonTerminals;
  std::unordered_set<std::string> terminals;
  std::string startSymbol;

  void addProduction(std::unique_ptr<Production> production);
  void setStartSymbol(const std::string& symbol);
};

}  // namespace bnf_calculator

#endif  // BNF_CALCULATOR_GRAMMAR_H