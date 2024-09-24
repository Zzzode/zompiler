if(ZOMPILER_ENABLE_ADDRESS_SANITIZER)
  add_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)

  # GCC on Linux does not automatically link libpthread when using ASAN
  if(UNIX
     AND NOT APPLE
     AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-lpthread)
  endif()
endif()

if(ZOMPILER_ENABLE_UNDEFINED_SANITIZER)
  add_compile_options(-fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()

if(ZOMPILER_ENABLE_WERROR)
  if(MSVC)
    add_compile_options(/WX)
  else()
    add_compile_options(-Werror)
  endif()
endif()
