file(GLOB TALCITE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB TALCITE_HEADERS CONFIGURE_DEPENDS "include/talcite/*.hpp")

add_library(talcite STATIC ${TALCITE_SOURCES} ${TALCITE_HEADERS})

target_include_directories(talcite PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

find_package(Threads REQUIRED)
target_link_libraries(talcite PUBLIC Threads::Threads)

# WNetGetConnectionW — wykaz dysków musi umieć powiedzieć, jaki udział stoi za literą.
# ws2_32 — gniazda, getaddrinfo i inet_pton mieszkają na Windows w Winsocku.
# iphlpapi — GetAdaptersAddresses, czyli wykaz własnych adresów dla local().
# bcrypt — skróty i HMAC bierzemy **od systemu**, przez CNG. To import do bcrypt.dll, obecnego
# w każdym Windowsie od Visty, tej samej klasy co ws2_32 i iphlpapi: nic się nie instaluje i nic
# nie trzeba dostarczać z programem. Na Linuksie ta sama rzecz idzie przez gniazdo AF_ALG i nie
# wymaga niczego dolinkowanego. Losowość nie potrzebuje żadnej z tych dróg — bierze ją
# std::random_device.
if(WIN32)
    target_link_libraries(talcite PUBLIC mpr ws2_32 iphlpapi bcrypt)
endif()
