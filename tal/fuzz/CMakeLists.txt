# The fuzz target of the JSON reader. Run it with its dictionary and a directory for the corpus,
# for example:
#
#   ./tal-json-fuzz -dict=json.dict -max_len=4096 corpus/
add_executable(tal-json-fuzz json.cpp)

target_link_libraries(tal-json-fuzz PRIVATE tal-core)

target_link_options(tal-json-fuzz PRIVATE -fsanitize=fuzzer)

configure_file(json.dict json.dict COPYONLY)
