# The fuzz targets of the JSON reader and of a program's texts. Run one with the dictionary and a
# directory for the corpus, for example:
#
#   ./tal-json-fuzz -dict=json.dict -max_len=4096 corpus/
add_executable(tal-json-fuzz json.cpp)

target_link_libraries(tal-json-fuzz PRIVATE tal-core)

target_link_options(tal-json-fuzz PRIVATE -fsanitize=fuzzer)

configure_file(json.dict json.dict COPYONLY)

add_executable(tal-texts-fuzz texts.cpp ../tools/generating.cpp)

target_include_directories(tal-texts-fuzz PRIVATE ../src ../tools)

target_link_libraries(tal-texts-fuzz PRIVATE tal-core)

target_link_options(tal-texts-fuzz PRIVATE -fsanitize=fuzzer)
