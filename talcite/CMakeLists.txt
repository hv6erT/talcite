file(GLOB TALCITE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB TALCITE_HEADERS CONFIGURE_DEPENDS "include/talcite/*.hpp")

add_library(talcite STATIC ${TALCITE_SOURCES} ${TALCITE_HEADERS})

target_include_directories(talcite PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Nadpisanie bez `override` (jak dawne Root::caret) widzi tylko -Wsuggest-override — budowanie
# bez flag milczy, a Clang z -Wall mówił. Przesłonięcia wirtualnej funkcji nazwą o innym podpisie
# (dawne Root::focus obok Node::focus) pilnuje -Woverloaded-virtual: na GCC poziom pierwszy, bo
# poziom drugi krzyczy o każdą z kilkudziesięciu klas, która nadpisuje jedno `react(…)`
# i przesłania z nazwy resztę przeciążeń `Handler` — a to jest kształt rozdania zdarzeń, nie błąd;
# na Clangu jedyny poziom, ciszę dają `using …::react` w klasach po KeyedNode i w Editor.
# PRIVATE — to reguła dla źródeł biblioteki, nie dla tych, którzy ją linkują. Poziom `=1` zna
# dopiero GCC 13; starszy (Debian 12, Ubuntu 22.04, RHEL 9) odrzucał całą budowę nieznaną opcją,
# więc tam ostrzeżenie dostaje sam `-Wsuggest-override`.
target_compile_options(talcite PRIVATE
  $<$<CXX_COMPILER_ID:GNU>:-Wsuggest-override>
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,13>>:-Woverloaded-virtual=1>
  $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wsuggest-override -Woverloaded-virtual>)

if(TALCITE_LEGACY_CONSOLE)
    target_compile_definitions(talcite PRIVATE TALCITE_LEGACY_CONSOLE)
endif()

find_package(Threads REQUIRED)
target_link_libraries(talcite PUBLIC Threads::Threads)

# WNetGetConnectionW — wykaz dysków musi umieć powiedzieć, jaki udział stoi za literą; z mpr także
# WNetAddConnection2W, czyli logowanie do udziału bez litery (U4).
# netapi32 — NetShareEnum, wykaz udziałów serwera (U4); CredWriteW z zapamiętaniem jest w advapi32.
# ws2_32 — gniazda, getaddrinfo i inet_pton mieszkają na Windows w Winsocku.
# iphlpapi — GetAdaptersAddresses, czyli wykaz własnych adresów dla local().
# bcrypt — skróty i HMAC bierzemy **od systemu**, przez CNG. To import do bcrypt.dll, obecnego
# w każdym Windowsie od Visty, tej samej klasy co ws2_32 i iphlpapi: nic się nie instaluje i nic
# nie trzeba dostarczać z programem. Na Linuksie ta sama rzecz idzie przez gniazdo AF_ALG i nie
# wymaga niczego dolinkowanego. Losowość nie potrzebuje żadnej z tych dróg — bierze ją
# std::random_device.
#
# advapi32 — listy dostępu do plików (prawa „tylko właściciel" z `fs::rights`); też w każdym Windowsie.
# crypt32 — sekret tylko dla tego użytkownika (`shielded`: CryptProtectData, DPAPI); w każdym Windowsie.
# shell32 i ole32 — foldery znane (`fs::folder`: SHGetKnownFolderPath i CoTaskMemFree) oraz
# argumenty programu w postaci szerokiej (`arguments`: CommandLineToArgvW).
if(WIN32)
    target_link_libraries(talcite PUBLIC mpr netapi32 ws2_32 iphlpapi bcrypt advapi32 shell32 ole32 crypt32)
endif()

# MinGW: biblioteki uruchomieniowe GCC — libstdc++, libgcc i winpthread — wchodzą do pliku
# wykonywalnego. Bez tego program zbudowany MinGW nie startuje na maszynie bez tych trzech DLL obok
# („nie znaleziono libstdc++-6.dll"), a MSVC tego problemu nie ma, bo bierze środowisko systemu.
# Flaga idzie do każdego programu, który linkuje talcite.
if(MINGW)
    target_link_libraries(talcite PUBLIC -static)
endif()
