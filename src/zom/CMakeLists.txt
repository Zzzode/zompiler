add_subdirectory(basic)
add_subdirectory(diagnostics)
add_subdirectory(lexer)
add_subdirectory(parser)
add_subdirectory(source)
add_subdirectory(typecheck)
add_subdirectory(zis)

add_library(
  compiler STATIC
  $<TARGET_OBJECTS:basic> $<TARGET_OBJECTS:diagnostics> $<TARGET_OBJECTS:lexer>
  $<TARGET_OBJECTS:parser> $<TARGET_OBJECTS:source> $<TARGET_OBJECTS:typecheck>)
target_link_libraries(compiler PUBLIC zc)