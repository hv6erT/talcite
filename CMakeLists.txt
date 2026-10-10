# 3.21 gives PROJECT_IS_TOP_LEVEL and the policies up to that version, including CMP0091, which
# selects the MSVC runtime library through CMAKE_MSVC_RUNTIME_LIBRARY. Visual Studio 2022 ships a
# newer CMake.
cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)

  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
      "$<$<CONFIG:Debug>:EditAndContinue>$<$<CONFIG:RelWithDebInfo>:ProgramDatabase>")
endif()

project(talcite CXX)

# A single-configuration generator (Make, Ninja) without a build type builds with neither
# optimization nor symbols, which suits neither a release nor a test run. The default is Release,
# but only when the project is built on its own; as a subdirectory it takes the type from the
# project that includes it.
get_property(multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(PROJECT_IS_TOP_LEVEL AND NOT multi AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
  add_compile_definitions(NOMINMAX)
endif()

if(MSVC)
  add_compile_options(/utf-8)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

foreach(chosen IN LISTS CMAKE_CONFIGURATION_TYPES)
  string(TOUPPER "${chosen}" loud)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${loud} "${CMAKE_BINARY_DIR}/bin")
endforeach()

option(TALCITE_TESTS "Build the tests" OFF)
option(TALCITE_DEMOS "Build the demo programs" ON)

# Translates the Windows console as if it did not accept VT sequences (Windows 7, 8.1, the legacy
# console), so that this path can be checked on Windows 10 and 11. Outside Windows it changes nothing.
option(TALCITE_LEGACY_CONSOLE "Translate the Windows console even when it accepts VT" OFF)
add_subdirectory(talcite)

# There are no adapters here, neither in the library nor next to it. talcite compiles and links
# without OpenSSL, and without an injected Seam<TlsProvider> no encrypted connection can be made, so
# "talcite does not implement TLS" is a fact rather than a promise. The adapters live in the
# separate project talcite-adapters.

if(TALCITE_DEMOS)
  add_subdirectory(demo)
endif()

if(TALCITE_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
