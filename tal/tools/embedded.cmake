# Files carried inside a program as bytes, for `tal_embedded`: each file becomes a constant
# `std::string_view` named after it ("pl-PL.json" is `pl_PL`, "default.json", a word of C++, is
# `default_`) in the namespace SPACE, declared in
# HEADER and defined in SOURCE. Every byte is written as an escape, sixteen to a line, so any
# content survives, zeros included. It runs as a CMake script, so a cross build makes it on the
# host as well.
#
# cmake -DSPACE=<namespace> -DHEADER=<file.hpp> -DSOURCE=<file.cpp> -DFILES=<a|b|...> -P embedded.cmake

string(REPLACE "|" ";" files "${FILES}")

# The words of C++ a name cannot be.
set(words alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t
  char32_t class compl concept const consteval constexpr constinit const_cast continue co_await
  co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern
  false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr
  operator or or_eq private protected public register reinterpret_cast requires return short signed
  sizeof static static_assert static_cast struct switch template this thread_local throw true try
  typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq)
get_filename_component(header_name "${HEADER}" NAME)

set(declared "#pragma once\n\n#include <string_view>\n\nnamespace ${SPACE} {\n\n")
set(defined "#include \"${header_name}\"\n\nnamespace ${SPACE} {\n")

foreach(file IN LISTS files)
  get_filename_component(stem "${file}" NAME_WE)
  string(MAKE_C_IDENTIFIER "${stem}" name)

  list(FIND words "${name}" word)

  if(word GREATER -1)
    string(APPEND name "_")
  endif()
  file(READ "${file}" hex HEX)
  string(LENGTH "${hex}" digits)
  math(EXPR size "${digits} / 2")

  # Two passes over the whole file rather than a loop over its lines, which would copy the file
  # once a line: every byte as an escape, then sixteen escapes (64 characters) to a line, and the
  # last, shorter line (none when the file's bytes come in whole sixteens) closed by hand.
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" escaped "${hex}")
  string(LENGTH "${escaped}" all)
  math(EXPR rest "${all} % 64")
  math(EXPR full "${all} - ${rest}")
  string(SUBSTRING "${escaped}" 0 ${full} body)
  string(SUBSTRING "${escaped}" ${full} -1 tail)
  string(REGEX REPLACE "(................................................................)" "    \"\\1\"\n" lines "${body}")

  if(rest GREATER 0)
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
