add_library(talcite-openssl STATIC sealed.cpp sealed.hpp)

target_include_directories(talcite-openssl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(talcite-openssl PUBLIC talcite OpenSSL::SSL OpenSSL::Crypto)

add_executable(talcite-openssl-check checking.cpp)

# Strażnik strony kodowej mieszka w demo/, bo `windows.h` nie ma prawa wejść do nagłówka talcite —
# a kopia tej samej klasy w drugim miejscu raz już nam się rozjechała.
target_include_directories(talcite-openssl-check PRIVATE "${CMAKE_SOURCE_DIR}/demo")

target_link_libraries(talcite-openssl-check PRIVATE talcite-openssl)
