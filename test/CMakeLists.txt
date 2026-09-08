file(GLOB TALCITE_TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

add_executable(talcite-test ${TALCITE_TEST_SOURCES})

# Prywatne nagłówki biblioteki są tu widoczne celowo: kodek STUN nie ma wyjścia na zewnątrz,
# a sprawdzić go trzeba — inaczej podpisy i odciski sprawdzałyby się dopiero w prawdziwej sieci.
target_include_directories(talcite-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                             "${CMAKE_SOURCE_DIR}/talcite/src")
target_link_libraries(talcite-test PRIVATE talcite)

# shell32 — CommandLineToArgvW. Argumenty podane przez `main` są na Windowsie przepisane na stronę
# kodową ANSI, a próby uruchamiania cudzych programów porównują bajty i muszą dostać UTF-8.
if(WIN32)
    target_link_libraries(talcite-test PRIVATE shell32)
endif()

add_test(NAME talcite COMMAND talcite-test)
