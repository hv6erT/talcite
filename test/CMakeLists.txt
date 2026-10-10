file(GLOB TALCITE_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(talcite-test ${TALCITE_TEST_SOURCES})

# The library's private headers are visible here on purpose: the STUN codec has no public
# interface, yet it must be tested, or its signatures and fingerprints would be checked only on a
# real network.
target_include_directories(talcite-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                             "${PROJECT_SOURCE_DIR}/talcite/src")
target_link_libraries(talcite-test PRIVATE talcite)

add_test(NAME talcite COMMAND talcite-test)
