file(GLOB TALCITE_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(talcite-test ${TALCITE_TEST_SOURCES})

# Prywatne nagłówki biblioteki są tu widoczne celowo: kodek STUN nie ma wyjścia na zewnątrz,
# a sprawdzić go trzeba — inaczej podpisy i odciski sprawdzałyby się dopiero w prawdziwej sieci.
target_include_directories(talcite-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                             "${PROJECT_SOURCE_DIR}/talcite/src")
target_link_libraries(talcite-test PRIVATE talcite)

add_test(NAME talcite COMMAND talcite-test)
