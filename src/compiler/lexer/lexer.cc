#include <cctype>
#include <iostream>
#include <string>
#include <tuple>

enum class TokenType {
  NUMBER,
  OPERATOR,
  LEFT_PAREN,
  RIGHT_PAREN,
  WHITESPACE,
  UNKNOWN
};

class Lexer {
 private:
  std::string input;
  size_t position;

 public:
  Lexer(const std::string &input) : input(input), position(0) {}
  std::pair<TokenType, std::string> getNextToken() {
    if (position >= input.length()) {
      return {TokenType::UNKNOWN, ""};
    }
    char c = input[position];
    if (std::isdigit(c)) {
      return readNumber();
    } else if (c == '+' || c == '-' || c == '*' || c == '/') {
      position++;
      return {TokenType::OPERATOR, std::string(1, c)};
    } else if (c == '(') {
      position++;
      return {TokenType::LEFT_PAREN, "("};
    } else if (c == ')') {
      position++;
      return {TokenType::RIGHT_PAREN, ")"};
    } else if (std::isspace(c)) {
      return readWhitespace();
    }
    position++;
    return {TokenType::UNKNOWN, std::string(1, c)};
  }

 private:
  std::pair<TokenType, std::string> readNumber() {
    std::string number;
    while (position < input.length() && std::isdigit(input[position])) {
      number += input[position];
      position++;
    }
    return {TokenType::NUMBER, number};
  }

  std::pair<TokenType, std::string> readWhitespace() {
    std::string whitespace;
    while (position < input.length() && std::isspace(input[position])) {
      whitespace += input[position];
      position++;
    }
    return {TokenType::WHITESPACE, whitespace};
  }
};

int main() {
  Lexer lexer("123 + 45 * (67 - 89) / 10");

  auto [type, value] = lexer.getNextToken();
  while (type != TokenType::UNKNOWN || !value.empty()) {
    std::cout << "Token: ";
    switch (type) {
      case TokenType::NUMBER:
        std::cout << "NUMBER";
        break;
      case TokenType::OPERATOR:
        std::cout << "OPERATOR";
        break;
      case TokenType::LEFT_PAREN:
        std::cout << "LEFT_PAREN";
        break;
      case TokenType::RIGHT_PAREN:
        std::cout << "RIGHT_PAREN";
        break;
      case TokenType::WHITESPACE:
        std::cout << "WHITESPACE";
        break;
      default:
        std::cout << "UNKNOWN";
        break;
    }
    std::cout << ", Value: \"" << value << "\"" << std::endl;

    std::tie(type, value) = lexer.getNextToken();
  }
  return 0;
}
