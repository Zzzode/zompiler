add_executable(bnf_calculator_tests lexer_test.cc parser_test.cc
                                    grammar_test.cc set_calculator_test.cc)

target_link_libraries(bnf_calculator_tests PRIVATE bnf_calculator)
add_test(NAME bnf_calculator_tests COMMAND bnf_calculator_tests)
