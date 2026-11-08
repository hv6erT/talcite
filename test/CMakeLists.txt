file(GLOB TALCITE_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

# The runner is tal::testing's, with the programs every suite pretends to be.
add_executable(talcite-test ${TALCITE_TEST_SOURCES} "${PROJECT_SOURCE_DIR}/tal/test/pretending.cpp")

# The library's private headers are visible here on purpose: the STUN codec has no public
# interface, yet it must be tested, or its signatures and fingerprints would be checked only on a
# real network.
target_include_directories(talcite-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                             "${PROJECT_SOURCE_DIR}/talcite/src")
target_link_libraries(talcite-test PRIVATE talcite tal-testing)

add_test(NAME talcite COMMAND talcite-test)
