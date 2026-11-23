# Files carried inside a program as bytes, for `tal_embedded`: each file becomes a constant
# `std::string_view` named after it ("pl-PL.json" is `pl_PL`) in the namespace SPACE, declared in
# HEADER and defined in SOURCE. Every byte is written as an escape, sixteen to a line, so any
# content survives, zeros included. It runs as a CMake script, so a cross build makes it on the
# host as well.
#
# cmake -DSPACE=<namespace> -DHEADER=<file.hpp> -DSOURCE=<file.cpp> -DFILES=<a|b|...> -P embedded.cmake

string(REPLACE "|" ";" files "${FILES}")
get_filename_component(header_name "${HEADER}" NAME)

set(declared "#pragma once\n\n#include <string_view>\n\nnamespace ${SPACE} {\n\n")
set(defined "#include \"${header_name}\"\n\nnamespace ${SPACE} {\n")

foreach(file IN LISTS files)
  get_filename_component(stem "${file}" NAME_WE)
  string(MAKE_C_IDENTIFIER "${stem}" name)
  file(READ "${file}" hex HEX)
  string(LENGTH "${hex}" digits)
  math(EXPR size "${digits} / 2")

  # Two passes over the whole file rather than a loop over its lines, which would copy the file
  # once a line: every byte as an escape, then sixteen escapes (64 characters) to a line, and the
  # last, shorter line closed by hand.
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" escaped "${hex}")
  string(REGEX REPLACE "(................................................................)" "    \"\\1\"\n" lines "${escaped}")
  string(REGEX MATCH "[^\n]*$" tail "${lines}")

  if(NOT tail STREQUAL "")
    string(LENGTH "${lines}" whole)
    string(LENGTH "${tail}" short)
    math(EXPR kept "${whole} - ${short}")
    string(SUBSTRING "${lines}" 0 ${kept} lines)
    string(APPEND lines "    \"${tail}\"\n")
  endif()

  if(lines STREQUAL "")
    set(lines "    \"\"\n")
  endif()

  string(APPEND declared "extern const std::string_view ${name};\n")
  string(APPEND defined "\nnamespace {\n\nconstexpr char ${name}_bytes[] =\n${lines};\n\n}\n\nconst std::string_view ${name}{${name}_bytes, ${size}};\n")
endforeach()

string(APPEND declared "\n}\n")
string(APPEND defined "\n}\n")

file(WRITE "${HEADER}" "${declared}")
file(WRITE "${SOURCE}" "${defined}")
