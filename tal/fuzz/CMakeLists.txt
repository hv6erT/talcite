# The fuzz targets of the JSON and XML readers, of inflate, deflate and the archives, of GIF, PNG
# (with our encoder) and JPEG, of the tags, of a program's texts and settings, of the diff, of the
# emulator of a terminal, of the answers of the HTTP client, of what a renderer's process says and
# of the text versions of HTML and of PDF, of drawing, of fonts and of plots. Inflate is checked
# against the system's zlib where the fuzz build finds one.
# Run one with the dictionary and a directory for the corpus, for example:
#
#   ./tal-json-fuzz -dict=json.dict -max_len=4096 corpus/
add_executable(tal-json-fuzz json.cpp)

target_link_libraries(tal-json-fuzz PRIVATE tal-core)

target_link_options(tal-json-fuzz PRIVATE -fsanitize=fuzzer)

configure_file(json.dict json.dict COPYONLY)

add_executable(tal-xml-fuzz xml.cpp)

target_link_libraries(tal-xml-fuzz PRIVATE tal-core)

target_link_options(tal-xml-fuzz PRIVATE -fsanitize=fuzzer)

configure_file(xml.dict xml.dict COPYONLY)

add_executable(tal-inflate-fuzz inflate.cpp)

target_include_directories(tal-inflate-fuzz PRIVATE ../src)

target_link_libraries(tal-inflate-fuzz PRIVATE tal-core)

target_link_options(tal-inflate-fuzz PRIVATE -fsanitize=fuzzer)

find_package(ZLIB)

if(ZLIB_FOUND)
  target_compile_definitions(tal-inflate-fuzz PRIVATE TAL_FUZZ_ZLIB)
  target_link_libraries(tal-inflate-fuzz PRIVATE ZLIB::ZLIB)
endif()

add_executable(tal-archive-fuzz archive.cpp)

target_link_libraries(tal-archive-fuzz PRIVATE tal-core)

target_link_options(tal-archive-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-png-fuzz png.cpp)

target_include_directories(tal-png-fuzz PRIVATE ../src)

target_link_libraries(tal-png-fuzz PRIVATE tal-media)

target_link_options(tal-png-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-jpeg-fuzz jpeg.cpp)

target_include_directories(tal-jpeg-fuzz PRIVATE ../src)

target_link_libraries(tal-jpeg-fuzz PRIVATE tal-media)

target_link_options(tal-jpeg-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-deflate-fuzz deflate.cpp)

target_include_directories(tal-deflate-fuzz PRIVATE ../src)

target_link_libraries(tal-deflate-fuzz PRIVATE tal-core)

target_link_options(tal-deflate-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-gif-fuzz gif.cpp)

target_include_directories(tal-gif-fuzz PRIVATE ../src)

target_link_libraries(tal-gif-fuzz PRIVATE tal-media)

target_link_options(tal-gif-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-tags-fuzz tags.cpp)

target_link_libraries(tal-tags-fuzz PRIVATE tal-media)

target_link_options(tal-tags-fuzz PRIVATE -fsanitize=fuzzer)

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

add_executable(tal-emulator-fuzz emulator.cpp)

target_link_libraries(tal-emulator-fuzz PRIVATE tal-ui)

target_link_options(tal-emulator-fuzz PRIVATE -fsanitize=fuzzer)

configure_file(emulator.dict emulator.dict COPYONLY)

add_executable(tal-http-fuzz http.cpp)

target_include_directories(tal-http-fuzz PRIVATE ../src)

target_link_libraries(tal-http-fuzz PRIVATE tal-net)

target_link_options(tal-http-fuzz PRIVATE -fsanitize=fuzzer)

configure_file(http.dict http.dict COPYONLY)

add_executable(tal-dropped-fuzz dropped.cpp)

target_include_directories(tal-dropped-fuzz PRIVATE ../src)

target_link_libraries(tal-dropped-fuzz PRIVATE tal-ui)

target_link_options(tal-dropped-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-rendered-fuzz rendered.cpp)

target_include_directories(tal-rendered-fuzz PRIVATE ../src)

target_link_libraries(tal-rendered-fuzz PRIVATE tal-media)

target_link_options(tal-rendered-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-htmltext-fuzz htmltext.cpp)

target_link_libraries(tal-htmltext-fuzz PRIVATE tal-ui tal-media)

target_link_options(tal-htmltext-fuzz PRIVATE -fsanitize=fuzzer)

configure_file(html.dict html.dict COPYONLY)

add_executable(tal-pdftext-fuzz pdftext.cpp)

target_link_libraries(tal-pdftext-fuzz PRIVATE tal-ui tal-media)

target_link_options(tal-pdftext-fuzz PRIVATE -fsanitize=fuzzer)

configure_file(pdf.dict pdf.dict COPYONLY)

add_executable(tal-drawing-fuzz drawing.cpp)

target_link_libraries(tal-drawing-fuzz PRIVATE tal-media)

target_link_options(tal-drawing-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-fonts-fuzz fonts.cpp)

target_link_libraries(tal-fonts-fuzz PRIVATE tal-media)

target_link_options(tal-fonts-fuzz PRIVATE -fsanitize=fuzzer)

add_executable(tal-plotting-fuzz plotting.cpp)

target_link_libraries(tal-plotting-fuzz PRIVATE tal-plotting)

target_link_options(tal-plotting-fuzz PRIVATE -fsanitize=fuzzer)
