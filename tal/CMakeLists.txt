# Fuzz targets for the readers of hostile text, for whoever works on them: libFuzzer drives them,
# AddressSanitizer and UBSan watch them. Everything in tal is instrumented, so that the fuzzer sees
# the coverage of all the code a text reaches. Clang only, and a build directory of its own.
option(TAL_FUZZ "Build the fuzz targets of tal (Clang only; instruments the whole build)" OFF)

if(TAL_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "TAL_FUZZ needs Clang, for libFuzzer.")
  endif()

  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# tal::core of talcite.next: tasks and their owners, executors, the clock, troubles, ports, the
# log, paths, the files port with its memory adapter, streams, the processes port with its
# scripted adapter, JSON, text (UTF-8, terminal cells, letter case, collation and the rules of
# languages), the dictionary of a program's texts, its settings, and in platform/ the system's adapters of the ports, built in as their defaults. It
# depends on nothing but the standard library and the system's own libraries.
find_package(Threads REQUIRED)

add_library(tal-core STATIC src/basics.cpp src/crypto.cpp src/executor.cpp src/fs.cpp src/memory.cpp src/path.cpp src/process.cpp src/stream.cpp src/sums.cpp src/task.cpp
  src/collate.cpp src/json.cpp src/language.cpp src/mending.cpp src/settings.cpp src/text.cpp src/texts.cpp src/unicode.cpp src/wording.cpp
  src/platform/bell.cpp src/platform/child_core.cpp src/platform/cipher_core.cpp src/platform/crypto_core.cpp src/platform/copy_core.cpp src/platform/disk.cpp src/platform/disk_core.cpp src/platform/file_core.cpp
  src/platform/language_core.cpp
  src/platform/trash_core.cpp src/platform/watch_core.cpp)
target_include_directories(tal-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tal-core PUBLIC Threads::Threads)
target_compile_features(tal-core PUBLIC cxx_std_20)

# Windows: the network connections of mapped drives (mpr), the shell's known folders and recycle
# bin (shell32, ole32), the rights of files (advapi32) and the digests and randomness of CNG
# (bcrypt), for platform/.
if(WIN32)
  target_link_libraries(tal-core PUBLIC mpr shell32 ole32 advapi32 bcrypt)
endif()

# What weakens security is built in only on the programmer's word: our own digests as the spare
# where the system gives none (TAL_OWN_SUMS), and MD5 and SHA-1, broken for collisions
# (TAL_WEAK_SUMS). Without them that code is not in the program.
option(TAL_OWN_SUMS "Our own digests where the system gives none" OFF)
option(TAL_WEAK_SUMS "The digests MD5 and SHA-1" OFF)

if(TAL_OWN_SUMS)
  target_compile_definitions(tal-core PUBLIC TAL_OWN_SUMS)
endif()

if(TAL_WEAK_SUMS)
  target_compile_definitions(tal-core PUBLIC TAL_WEAK_SUMS)
endif()

# tal-texts, the generator of a program's texts, run while the program is built: out of the
# English texts it makes C++ functions, one for each text, and it checks every other language
# against English. A cross build cannot run what it builds, so there TAL_TEXTS names tal-texts
# built for the host.
add_executable(tal-texts tools/texts.cpp tools/files.cpp tools/generating.cpp)
target_include_directories(tal-texts PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tal-texts PRIVATE tal-core)

set(TAL_TEXTS "" CACHE FILEPATH "tal-texts built for the host, for a cross build")

# tal-settings, the generator of a program's settings, run while the program is built: out of the
# built-in values it makes a handle with its type for each setting, and the function that gives
# the values. In a cross build TAL_SETTINGS names it built for the host.
add_executable(tal-settings tools/settings.cpp tools/files.cpp tools/generating.cpp)
target_include_directories(tal-settings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tal-settings PRIVATE tal-core)

set(TAL_SETTINGS "" CACHE FILEPATH "tal-settings built for the host, for a cross build")

# tal_texts(<target> <en-US.json> <namespace>): the texts of <target>. The functions go into
# <namespace>.hpp and .cpp in the build directory, made again when English changes; every language
# beside English is checked whenever one of them changes, and a problem stops the build with its
# file, line and column.
function(tal_texts target english space)
  get_filename_component(english "${english}" ABSOLUTE)
  get_filename_component(folder "${english}" DIRECTORY)
  string(REPLACE "::" ";" parts "${space}")
  list(GET parts -1 name)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/tal-texts/${target}")

  if(CMAKE_CROSSCOMPILING)
    if(NOT TAL_TEXTS)
      message(FATAL_ERROR "A cross build runs the generator of texts on the host: set TAL_TEXTS to tal-texts built for it.")
    endif()

    set(generator "${TAL_TEXTS}")
  else()
    set(generator tal-texts)
  endif()

  file(GLOB languages CONFIGURE_DEPENDS "${folder}/*.json")

  add_custom_command(OUTPUT "${out}/${name}.hpp" "${out}/${name}.cpp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
    COMMAND ${generator} generate "${english}" "${space}" "${out}/${name}.hpp" "${out}/${name}.cpp"
    DEPENDS "${english}" ${generator}
    COMMENT "Making the texts of ${target}"
    VERBATIM)

  add_custom_command(OUTPUT "${out}/checked"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
    COMMAND ${generator} check "${out}/checked" "${english}" ${languages}
    DEPENDS ${languages} ${generator}
    COMMENT "Checking the languages of ${target}"
    VERBATIM)

  target_sources(${target} PRIVATE "${out}/${name}.hpp" "${out}/${name}.cpp" "${out}/checked")
  target_include_directories(${target} PUBLIC "${out}")
  target_link_libraries(${target} PUBLIC tal-core)
endfunction()

# tal_settings(<target> <defaults.json> <namespace>): the settings of <target>. The handles and
# the built-in values go into <namespace>.hpp and .cpp in the build directory, made again when the
# built-in values change; a problem stops the build with its file, line and column.
function(tal_settings target defaults space)
  get_filename_component(defaults "${defaults}" ABSOLUTE)
  string(REPLACE "::" ";" parts "${space}")
  list(GET parts -1 name)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/tal-settings/${target}")

  if(CMAKE_CROSSCOMPILING)
    if(NOT TAL_SETTINGS)
      message(FATAL_ERROR "A cross build runs the generator of settings on the host: set TAL_SETTINGS to tal-settings built for it.")
    endif()

    set(generator "${TAL_SETTINGS}")
  else()
    set(generator tal-settings)
  endif()

  add_custom_command(OUTPUT "${out}/${name}.hpp" "${out}/${name}.cpp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
    COMMAND ${generator} generate "${defaults}" "${space}" "${out}/${name}.hpp" "${out}/${name}.cpp"
    DEPENDS "${defaults}" ${generator}
    COMMENT "Making the settings of ${target}"
    VERBATIM)

  target_sources(${target} PRIVATE "${out}/${name}.hpp" "${out}/${name}.cpp")
  target_include_directories(${target} PUBLIC "${out}")
  target_link_libraries(${target} PUBLIC tal-core)
endfunction()

# tal::ui of talcite.next: the geometry of cells, looks and style sheets with their selectors
# and the built-in sheet, the surface of cells a frame is painted on, keys and events, and the
# tree of nodes with its document, which delivers events, keeps the focus and the timers, and
# styles, lays out and paints what changed.
add_library(tal-ui STATIC src/action.cpp src/app.cpp src/capabilities.cpp src/clipboard.cpp src/conhost.cpp src/console.cpp src/damage.cpp src/decoder.cpp src/diff.cpp src/document.cpp src/emulator.cpp src/terminal.cpp src/editing.cpp src/event.cpp src/fields.cpp
  src/controls.cpp src/geometry.cpp src/key.cpp src/layout.cpp src/menu.cpp src/model.cpp src/pages.cpp src/editor.cpp src/lining.cpp src/dropping.cpp src/spooling.cpp src/node.cpp src/style.cpp src/surface.cpp src/undo.cpp src/views.cpp src/vt.cpp src/window.cpp
  src/platform/console_core.cpp)
target_link_libraries(tal-ui PUBLIC tal-core)

# Windows: the double-click time (user32) and NormalizeString (normaliz), which composes a letter
# with its marks for the console without VT, whose cell holds one UTF-16 unit.
if(WIN32)
  target_link_libraries(tal-ui PUBLIC user32 normaliz)
endif()

# tal::net of talcite.next: addresses and names, TCP as a stream, listeners and UDP, waited for by
# one reactor thread for the whole program: epoll on Linux, kqueue on macOS and the BSDs, IOCP on
# Windows, poll elsewhere. TAL_NET_KQUEUE takes kqueue where the system has another, as Linux has
# through libkqueue, so that its backend is tried there too. TLS over any stream, and an HTTP client
# with the proxy of the environment or, on Windows, of WinHTTP's setting.
option(TAL_NET_KQUEUE "Wait for sockets through kqueue (libkqueue on Linux)" OFF)

add_library(tal-net STATIC src/net.cpp src/tls.cpp src/http.cpp src/platform/iocp_core.cpp src/platform/reactor_core.cpp src/platform/sockets_core.cpp src/platform/tls_core.cpp src/platform/proxy_core.cpp)
target_link_libraries(tal-net PUBLIC tal-core)

if(WIN32)
  target_link_libraries(tal-net PUBLIC ws2_32 mswsock secur32 crypt32 ncrypt winhttp)
endif()

if(TAL_NET_KQUEUE)
  target_compile_definitions(tal-net PUBLIC TAL_NET_KQUEUE)

  if(NOT CMAKE_SYSTEM_NAME MATCHES "BSD|Darwin")
    find_path(TAL_KQUEUE_INCLUDE sys/event.h PATH_SUFFIXES kqueue REQUIRED)
    find_library(TAL_KQUEUE_LIBRARY kqueue REQUIRED)
    target_include_directories(tal-net PRIVATE "${TAL_KQUEUE_INCLUDE}")
    target_link_libraries(tal-net PUBLIC "${TAL_KQUEUE_LIBRARY}")
  endif()
endif()

# tal::testing: the runner of the trials with its watch, which writes the stacks of a hung run's
# threads (dbghelp on Windows; on Linux the trial binaries export their symbols, so that the
# stacks name the functions of the trials too).
add_library(tal-testing STATIC src/testing.cpp src/watch.cpp)
target_link_libraries(tal-testing PUBLIC tal-core)

if(WIN32)
  target_link_libraries(tal-testing PUBLIC dbghelp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(tal-testing PUBLIC ${CMAKE_DL_LIBS})
  target_link_options(tal-testing INTERFACE -rdynamic)
endif()

# tal::testing::Scene: a document driven by hand on a manual clock, for the trials of programs.
add_library(tal-scene STATIC src/scene.cpp)
target_link_libraries(tal-scene PUBLIC tal-ui)

if(TALCITE_TESTS)
  # The programs the trial binaries pretend to be (pretending.cpp) are shared by every suite.
  file(GLOB TAL_TEST_SOURCES CONFIGURE_DEPENDS "test/*.cpp")
  add_executable(tal-test ${TAL_TEST_SOURCES} tools/generating.cpp)
  target_link_libraries(tal-test PRIVATE tal-testing tal-scene tal-net)
  # Some trials check internal parts (the console translation without VT, the generator of texts)
  # through their headers.
  target_include_directories(tal-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tools")
  # The texts of the trials, made and checked as a program's are.
  tal_texts(tal-test test/texts/en-US.json spoken)
  tal_settings(tal-test test/settings/defaults.json preset)
  target_compile_definitions(tal-test PRIVATE TAL_TEST_TEXTS="${CMAKE_CURRENT_SOURCE_DIR}/test/texts")
  add_test(NAME tal COMMAND tal-test)
endif()

if(TAL_FUZZ)
  add_subdirectory(fuzz)
endif()
