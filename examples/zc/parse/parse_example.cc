#include "src/zc/base/common.h"
#include "src/zc/base/debug.h"
#include "src/zc/base/function.h"
#include "src/zc/base/main.h"
#include "src/zc/memory/arena.h"
#include "src/zc/parse/char.h"
#include "src/zc/parse/common.h"
#include "src/zc/strings/string.h"

namespace examples {

namespace p = zc::parse;

// <expression> ::= <term> { <addop> <term> }
// <term> ::= <factor> { <mulop> <factor> }
// <factor> ::= <number> | "(" <expression> ")"
// <addop> ::= "+" | "-"
// <mulop> ::= "*" | "/"
// <number> ::= <digit>+ [ "." <digit>* ]
// <digit> ::= "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"

class ExpressionParser {
  using ParserInput = p::IteratorInput<char, const char*>;

 public:
  ExpressionParser() {
    auto& expression = expression_;
    auto& factor = arena_.copy(p::oneOf(
        p::number, p::transform(p::sequence(p::exactChar<'('>(), expression,
                                            p::exactChar<')'>()),
                                [](double f) {
                                  ZC_LOG(DBG, "factor");
                                  return f;
                                })));
    auto& addop = arena_.copy(p::oneOf(constResult(p::exactly('+'), '+'),
                                       constResult(p::exactly('-'), '-')));
    auto& mulop = arena_.copy(p::oneOf(constResult(p::exactly('*'), '*'),
                                       constResult(p::exactly('/'), '/')));
    auto& term = arena_.copy(p::transform(
        p::sequence(factor, p::many(p::sequence(mulop, factor))),
        [](double f, const zc::Array<zc::Tuple<char, double>>& res) {
          ZC_LOG(DBG, "in term");

          for (auto& m : res) {
            const char op = zc::get<0>(m);
            if (op == '*') {
              ZC_LOG(DBG, "aaaa");
              f *= zc::get<1>(m);
            } else {
              ZC_LOG(DBG, "bbbb");
              f /= zc::get<1>(m);
            }
          }
          return f;
        }));
    expression_ = arena_.copy(p::transform(
        p::sequence(term, p::many(p::sequence(addop, term))),
        [](double f, const zc::Array<zc::Tuple<char, double>>& res) -> double {
          ZC_LOG(DBG, "in expression_");

          for (auto& m : res) {
            const char op = zc::get<0>(m);
            if (op == '+') {
              ZC_LOG(DBG, "cccc");
              f += zc::get<1>(m);
            } else {
              ZC_LOG(DBG, "dddd");
              f -= zc::get<1>(m);
            }
          }
          return f;
        }));
  }

  zc::Maybe<double> parse(zc::StringPtr input) {
    ParserInput parserInput(input.begin(), input.end());
    return expression_(parserInput);
  }

 private:
  zc::Arena arena_;
  p::ParserRef<ParserInput, double> expression_;
};

}  // namespace examples

// 主类定义
class MainClass {
 public:
  explicit MainClass(zc::ProcessContext& context) : context_(context) {}

  zc::MainBuilder::Validity SetExpression(zc::StringPtr expr) {
    expression_ = expr;
    return true;
  }

  zc::MainFunc getMain() {
    return zc::MainBuilder(
               context_, "Expression Calculator v1.0",
               "Calculates the result of an addition/subtraction expression.")
        .expectOneOrMoreArgs("<expression>",
                             ZC_BIND_METHOD(*this, SetExpression))
        .addOption(
            {'d', "detail"},
            [this]() {
              verbose_ = true;
              return true;
            },
            "Enable detailed output.")
        .callAfterParsing(ZC_BIND_METHOD(*this, Calculate))
        .build();
  }

 private:
  zc::MainBuilder::Validity Calculate() {
    if (expression_ == nullptr) {
      return "No expression provided.";
    }

    examples::ExpressionParser parser;
    ZC_IF_SOME(result, parser.parse(expression_)) {
      if (verbose_) {
        context_.exitInfo(
            zc::str("Expression: ", expression_, "\nResult: ", result));
      } else {
        context_.exitInfo(zc::str(result));
      }
    }
    else {
      return "Failed to parse the expression.";
    }

    return true;
  }

  zc::ProcessContext& context_;
  zc::StringPtr expression_;
  bool verbose_ = false;
};

ZC_MAIN(MainClass)