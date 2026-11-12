# tal::core of talcite.next: tasks and their owners, executors, the clock, troubles, ports, the
# log, paths, the files port with its memory adapter, streams, the processes port with its
# scripted adapter, text (UTF-8, terminal cells, letter case, collation and the rules of
# languages), and in platform/ the system's adapters of the ports, built in as their defaults. It
# depends on nothing but the standard library and the system's own libraries.
find_package(Threads REQUIRED)

add_library(tal-core STATIC src/basics.cpp src/executor.cpp src/fs.cpp src/memory.cpp src/path.cpp src/process.cpp src/stream.cpp src/task.cpp
  src/collate.cpp src/language.cpp src/text.cpp src/unicode.cpp
  src/platform/bell.cpp src/platform/child_core.cpp src/platform/copy_core.cpp src/platform/disk.cpp src/platform/disk_core.cpp src/platform/file_core.cpp
  src/platform/language_core.cpp
  src/platform/trash_core.cpp src/platform/watch_core.cpp)
target_include_directories(tal-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tal-core PUBLIC Threads::Threads)
target_compile_features(tal-core PUBLIC cxx_std_20)

# Windows: the network connections of mapped drives (mpr), the shell's known folders and recycle
# bin (shell32, ole32) and the rights of files (advapi32), for platform/.
if(WIN32)
  target_link_libraries(tal-core PUBLIC mpr shell32 ole32 advapi32)
endif()

# tal::ui of talcite.next: the geometry of cells, looks and style sheets with their selectors
# and the built-in sheet, the surface of cells a frame is painted on, keys and events, and the
# tree of nodes with its document, which delivers events, keeps the focus and the timers, and
# styles, lays out and paints what changed.
add_library(tal-ui STATIC src/action.cpp src/app.cpp src/capabilities.cpp src/clipboard.cpp src/conhost.cpp src/console.cpp src/damage.cpp src/decoder.cpp src/document.cpp src/editing.cpp src/event.cpp src/fields.cpp
  src/controls.cpp src/geometry.cpp src/key.cpp src/layout.cpp src/menu.cpp src/model.cpp src/pages.cpp src/editor.cpp src/spooling.cpp src/node.cpp src/reading.cpp src/style.cpp src/surface.cpp src/undo.cpp src/views.cpp src/vt.cpp src/window.cpp
  src/platform/console_core.cpp)
target_link_libraries(tal-ui PUBLIC tal-core)

# Windows: the double-click time (user32) and NormalizeString (normaliz), which composes a letter
# with its marks for the console without VT, whose cell holds one UTF-16 unit.
if(WIN32)
  target_link_libraries(tal-ui PUBLIC user32 normaliz)
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
  add_executable(tal-test ${TAL_TEST_SOURCES})
  target_link_libraries(tal-test PRIVATE tal-testing tal-scene)
  # Some trials check internal parts (the console translation without VT) through their headers.
  target_include_directories(tal-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  add_test(NAME tal COMMAND tal-test)
endif()
