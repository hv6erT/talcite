cmake_minimum_required(VERSION 3.10)

if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)

  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
      "$<$<CONFIG:Debug>:EditAndContinue>$<$<CONFIG:RelWithDebInfo>:ProgramDatabase>")
endif()

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

project(talcite CXX)

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

option(TALCITE_TESTS "Buduj przypadki testowe" OFF)
option(TALCITE_DEMOS "Buduj programy pokazowe" ON)
option(TALCITE_OPENSSL "Buduj przejściówkę do OpenSSL" OFF)

add_subdirectory(talcite)

# Przejściówka **nie należy do biblioteki**. talcite kompiluje się i linkuje bez OpenSSL, a bez
# wstrzykniętego Guard nie da się zbudować połączenia szyfrowanego — dzięki temu „nie piszemy
# kryptografii" jest faktem, a nie obietnicą.
if(TALCITE_OPENSSL)
  find_package(OpenSSL REQUIRED)

  add_subdirectory(extra/openssl)
endif()

if(TALCITE_DEMOS)
  add_subdirectory(demo)
endif()

if(TALCITE_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
