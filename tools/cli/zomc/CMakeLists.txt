if(BUILD_CLI)
  add_executable(zomc main.cc)
  target_link_libraries(zomc PRIVATE zc compiler)
endif()
