# 3.21: `PROJECT_IS_TOP_LEVEL` i polityki do tej wersji włącznie — w tym CMP0091, czyli wybór
# biblioteki uruchomieniowej MSVC przez CMAKE_MSVC_RUNTIME_LIBRARY. Visual Studio 2022 ma nowszy.
cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)

  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
      "$<$<CONFIG:Debug>:EditAndContinue>$<$<CONFIG:RelWithDebInfo>:ProgramDatabase>")
endif()

project(talcite CXX)

# Generator jednokonfiguracyjny (Make, Ninja) bez podanego typu budował bez optymalizacji i bez
# symboli — ani wydanie, ani próba. Domyślnie Release; tylko gdy projekt buduje się sam, bo jako
# podkatalog bierze typ od tego, kto go włącza.
get_property(multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(PROJECT_IS_TOP_LEVEL AND NOT multi AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Typ budowania" FORCE)
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

option(TALCITE_TESTS "Buduj przypadki testowe" OFF)
option(TALCITE_DEMOS "Buduj programy pokazowe" ON)

# Konsola Windows tłumaczona tak, jakby VT nie przyjmowała (Windows 7, 8.1, „starsza konsola") —
# żeby tę drogę dało się sprawdzić na Windows 10 i 11. Poza Windows nic nie zmienia.
option(TALCITE_LEGACY_CONSOLE "Tłumacz konsolę Windows także wtedy, gdy przyjmuje VT" OFF)
add_subdirectory(talcite)

# Przejściówek tu **nie ma** — ani w bibliotece, ani obok niej. talcite kompiluje się i linkuje bez
# OpenSSL, a bez wstrzykniętego Seam<TlsProvider> nie da się zbudować połączenia szyfrowanego —
# dzięki temu „nie piszemy kryptografii" jest faktem, a nie obietnicą. Przejściówki mieszkają
# w osobnym projekcie talcite-adapters (dawniej extra/openssl tutaj).

if(TALCITE_DEMOS)
  add_subdirectory(demo)
endif()

if(TALCITE_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
