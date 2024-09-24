#include "src/bnf_calculator/calculator.h"

namespace bnf_calculator {

Calculator::Calculator(const Grammar& grammar) : grammar(grammar) {}

Calculator::SetMap Calculator::computeFirstSets() {
  initializeFirstSets();
  bool changed;
  do {
    changed = false;
    for (const auto& production : grammar.rules) {
      const auto& nonTerminal = production->lhs->name;
      for (const auto& alternative : production->rhs) {
        auto firstOfRHS = computeFirstSetOfSequence(alternative);
        size_t originalSize = firstSets[nonTerminal].size();
        firstSets[nonTerminal].insert(firstOfRHS.begin(), firstOfRHS.end());
        if (firstSets[nonTerminal].size() > originalSize) {
          changed = true;
        }
      }
    }
  } while (changed);

  return firstSets;
}

Calculator::SetMap Calculator::computeFollowSets() {
  initializeFollowSets();
  bool changed;
  do {
    changed = false;
    for (const auto& production : grammar.rules) {
      const auto& lhs = production->lhs->name;
      for (const auto& alternative : production->rhs) {
        for (size_t i = 0; i < alternative.size(); ++i) {
          const auto& symbol = alternative[i]->name;
          if (!alternative[i]->isTerminal) {
            SymbolSet firstOfRest;
            if (i < alternative.size() - 1) {
              firstOfRest = computeFirstSetOfSequence(alternative, i + 1);
            }
            size_t originalSize = followSets[symbol].size();
            followSets[symbol].insert(firstOfRest.begin(), firstOfRest.end());
            if (i == alternative.size() - 1 || isNullable(alternative, i + 1)) {
              followSets[symbol].insert(followSets[lhs].begin(),
                                        followSets[lhs].end());
            }
            if (followSets[symbol].size() > originalSize) {
              changed = true;
            }
          }
        }
      }
    }
  } while (changed);

  return followSets;
}

std::unordered_map<std::string, Calculator::SetMap>
Calculator::computeSelectSets() {
  std::unordered_map<std::string, SetMap> selectSets;

  for (const auto& production : grammar.rules) {
    const auto& lhs = production->lhs->name;
    for (const auto& alternative : production->rhs) {
      SymbolSet select;
      auto first = computeFirstSetOfSequence(alternative);
      select.insert(first.begin(), first.end());
      if (isNullable(alternative)) {
        const auto& follow = followSets[lhs];
        select.insert(follow.begin(), follow.end());
      }
      selectSets[lhs][lhs + " -> " + symbolSequenceToString(alternative)] =
          select;
    }
  }

  return selectSets;
}

void Calculator::initializeFirstSets() {
  for (const auto& terminal : grammar.terminals) {
    firstSets[terminal].insert(terminal);
  }
  for (const auto& nonTerminal : grammar.nonTerminals) {
    firstSets[nonTerminal] = {};
  }
}

void Calculator::initializeFollowSets() {
  for (const auto& nonTerminal : grammar.nonTerminals) {
    followSets[nonTerminal] = {};
  }
  followSets[grammar.startSymbol].insert(
      "$");  // Add end-of-input symbol to start symbol's FOLLOW set
}

Calculator::SymbolSet Calculator::computeFirstSetOfSequence(
    const std::vector<std::unique_ptr<Symbol>>& sequence, size_t start) {
  SymbolSet result;
  for (size_t i = start; i < sequence.size(); ++i) {
    const auto& symbol = sequence[i];
    const auto& symbolName = symbol->name;
    if (symbol->isTerminal) {
      result.insert(symbolName);
      break;
    } else {
      result.insert(firstSets[symbolName].begin(), firstSets[symbolName].end());
      if (!isNullable(symbolName)) {
        break;
      }
    }
  }
  return result;
}

bool Calculator::isNullable(const std::string& symbol) const {
  if (grammar.terminals.find(symbol) != grammar.terminals.end()) {
    return false;
  }
  for (const auto& production : grammar.rules) {
    if (production->lhs->name == symbol) {
      for (const auto& alternative : production->rhs) {
        if (isNullable(alternative)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool Calculator::isNullable(
    const std::vector<std::unique_ptr<Symbol>>& sequence, size_t start) const {
  for (size_t i = start; i < sequence.size(); ++i) {
    if (!isNullable(sequence[i]->name)) {
      return false;
    }
  }
  return true;
}

std::string Calculator::symbolSequenceToString(
    const std::vector<std::unique_ptr<Symbol>>& sequence) const {
  std::string result;
  for (const auto& symbol : sequence) {
    if (!result.empty()) {
      result += " ";
    }
    result += symbol->name;
  }
  return result;
}

}  // namespace bnf_calculator