cmake_minimum_required(VERSION 3.24)
project(astra LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GF_BUILD_GUI "Build Qt GUI app" ON)
option(GF_BUILD_TESTS "Build tests" ON)

include(cmake/gf_version.cmake)
include(cmake/gf_options.cmake)
include(cmake/gf_warnings.cmake)

include(FetchContent)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.14.1
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

if (GF_BUILD_TESTS)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.6.0
  )
  FetchContent_MakeAvailable(Catch2)
endif()

# Generated headers
include_directories(${CMAKE_BINARY_DIR}/generated)

add_subdirectory(libs)
add_subdirectory(apps)

if (GF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()