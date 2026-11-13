# The fuzz targets of the JSON reader, of a program's texts and settings, and of the diff. Run one
# with the dictionary and a directory for the corpus, for example:
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

add_executable(tal-settings-fuzz settings.cpp ../tools/generating.cpp)

target_include_directories(tal-settings-fuzz PRIVATE ../src ../tools)

target_link_libraries(tal-settings-fuzz PRIVATE tal-core)

target_link_options(tal-settings-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-diff-fuzz diff.cpp)

target_link_libraries(tal-diff-fuzz PRIVATE tal-ui)

target_link_options(tal-diff-fuzz PRIVATE -fsanitize=fuzzer)
