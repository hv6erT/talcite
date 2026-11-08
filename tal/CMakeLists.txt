# tal::core of talcite.next: tasks and their owners, executors, the clock, troubles, ports and the
# log. It depends on nothing but the standard library and the system's threads.
find_package(Threads REQUIRED)

add_library(tal-core STATIC src/basics.cpp src/executor.cpp src/task.cpp)
target_include_directories(tal-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tal-core PUBLIC Threads::Threads)
target_compile_features(tal-core PUBLIC cxx_std_20)

# tal::testing: what trials need beyond core; it grows its own runner in E2.
add_library(tal-testing STATIC src/testing.cpp)
target_link_libraries(tal-testing PUBLIC tal-core)

if(TALCITE_TESTS)
  # The trials run on the runner of talcite's own trials until tal::testing brings its own (E2).
  file(GLOB TAL_TEST_SOURCES CONFIGURE_DEPENDS "test/*.cpp")
  add_executable(tal-test ${TAL_TEST_SOURCES} "${PROJECT_SOURCE_DIR}/test/proving.cpp")
  target_include_directories(tal-test PRIVATE "${PROJECT_SOURCE_DIR}/test")
  target_link_libraries(tal-test PRIVATE tal-testing talcite)
  add_test(NAME tal COMMAND tal-test)
endif()
