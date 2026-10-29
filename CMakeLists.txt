# 3.21 gives PROJECT_IS_TOP_LEVEL and the policies up to that version, including CMP0091, which
# selects the MSVC runtime library through CMAKE_MSVC_RUNTIME_LIBRARY. Visual Studio 2022 ships a
# newer CMake.
cmake_minimum_required(VERSION 3.21)

# A Debug build puts its debug information into the object files (/Z7) instead of a program
# database written through the mspdbsrv server (/ZI): the server outlives a build in Visual Studio,
# and its heap grows from build to build until the compiler runs out of heap space (C1060), however
# much memory is free. The price is Edit and Continue; the linker still writes the .pdb.
if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)

  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
      "$<$<CONFIG:Debug>:Embedded>$<$<CONFIG:RelWithDebInfo>:ProgramDatabase>")
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

# A compiler working on a large source takes gigabytes of memory, and Ninja starts as many compilers
# as there are cores: with many cores and not as much memory they run out of it together (MSVC stops
# with C1060, out of heap space, however much memory the system shows free). So compiling goes
# through a pool sized by the memory: one compiler per TALCITE_COMPILE_MEMORY_MIB of physical memory,
# at least one. Only the project built on its own sets the pool, for its subdirectories too; only
# Ninja has pools, and other generators ignore them.
if(PROJECT_IS_TOP_LEVEL)
  set(TALCITE_COMPILE_MEMORY_MIB 2048 CACHE STRING "Memory one compiler may take, in MiB; sizes the pool of compiling jobs")

  if(NOT TALCITE_COMPILE_MEMORY_MIB MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "TALCITE_COMPILE_MEMORY_MIB must be a whole number of MiB greater than zero, not \"${TALCITE_COMPILE_MEMORY_MIB}\".")
  endif()

  cmake_host_system_information(RESULT memory QUERY TOTAL_PHYSICAL_MEMORY)
  math(EXPR compilers "${memory} / ${TALCITE_COMPILE_MEMORY_MIB}")

  if(compilers LESS 1)
    set(compilers 1)
  endif()

  set_property(GLOBAL APPEND PROPERTY JOB_POOLS compiling=${compilers})
  set(CMAKE_JOB_POOL_COMPILE compiling)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
  add_compile_definitions(NOMINMAX)
endif()

# A COFF object file holds at most 65,279 sections, and a large translation unit full of templates
# (a test file with many trials, the matching code) goes past that in a Debug build: MSVC stops with
# C1128, MinGW's assembler with "too many sections". /bigobj and -mbig-obj lift the limit; the
# object files only grow a little.
if(MSVC)
  add_compile_options(/utf-8 /bigobj)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-Wa,-mbig-obj)
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
