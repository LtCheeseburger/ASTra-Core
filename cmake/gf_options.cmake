# Common compile options
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
