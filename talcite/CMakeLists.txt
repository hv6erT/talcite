file(GLOB TALCITE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB TALCITE_HEADERS CONFIGURE_DEPENDS "include/talcite/*.hpp")

add_library(talcite STATIC ${TALCITE_SOURCES} ${TALCITE_HEADERS})

target_include_directories(talcite PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# An override without the override keyword is caught only by -Wsuggest-override; a build without
# flags stays silent. Hiding a virtual function behind a name with a different signature is caught
# by -Woverloaded-virtual: level 1 on GCC, because level 2 complains about each of the dozens of
# classes that override one react(...) and so hide the other Handler overloads by name, which is
# how events are dispatched, not a mistake. Clang has a single level; the using ...::react
# declarations in the classes derived from KeyedMixin and in Editor keep it quiet.
# PRIVATE: this is a rule for the library's sources, not for those who link it. Level =1 exists
# only from GCC 13; older GCC (Debian 12, Ubuntu 22.04, RHEL 9) rejects the whole build over an
# unknown option, so there the warning comes from -Wsuggest-override alone.
target_compile_options(talcite PRIVATE
  $<$<CXX_COMPILER_ID:GNU>:-Wsuggest-override>
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,13>>:-Woverloaded-virtual=1>
  $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wsuggest-override -Woverloaded-virtual>)

if(TALCITE_LEGACY_CONSOLE)
    target_compile_definitions(talcite PRIVATE TALCITE_LEGACY_CONSOLE)
endif()

find_package(Threads REQUIRED)
target_link_libraries(talcite PUBLIC Threads::Threads)

# mpr: WNetGetConnectionW, so that the list of drives can tell which share stands behind a letter,
# and WNetAddConnection2W, which logs in to a share without a drive letter.
# netapi32: NetShareEnum, the list of a server's shares. CredWriteW, which remembers a login, is in
# advapi32.
# ws2_32: on Windows, sockets, getaddrinfo and inet_pton live in Winsock.
# iphlpapi: GetAdaptersAddresses, the list of the machine's own addresses for local().
# bcrypt: digests, HMAC and random bytes come from the system, through CNG. bcrypt.dll is present
# in every Windows since Vista, like ws2_32 and iphlpapi, so nothing is installed and nothing has
# to ship with the program. On Linux digests go through an AF_ALG socket and need nothing linked.
#
# advapi32: access lists of files (owner-only permissions from `fs::rights`); in every Windows.
# crypt32: a secret readable only by this user (`shielded`: CryptProtectData, DPAPI); in every
# Windows.
# shell32 and ole32: known folders (`fs::folder`: SHGetKnownFolderPath and CoTaskMemFree), the
# program's arguments in wide form (`arguments`: CommandLineToArgvW), and opening an address in its
# program (`launcher`: ShellExecuteW, with COM around it).
# normaliz: NormalizeString, which composes a letter with its marks for the old console, whose cell
# holds one UTF-16 unit; present since Vista.
# shlwapi and uuid: the desktop (`desktop`) asks for the default program of a type
# (AssocQueryStringW) and binds a file to a data object for a program's handler; uuid holds the
# interface and handler identifiers (IID_IShellItem, BHID_DataObject). Both in every Windows.
if(WIN32)
    target_link_libraries(talcite PUBLIC mpr netapi32 ws2_32 iphlpapi bcrypt advapi32 shell32 ole32 crypt32 normaliz shlwapi uuid)
endif()

# CoreServices: Launch Services, which the desktop (`desktop`) asks for the programs of a file on
# macOS.
if(APPLE)
    target_link_libraries(talcite PUBLIC "-framework CoreServices")
endif()

# MinGW: the GCC runtime libraries (libstdc++, libgcc and winpthread) go into the executable.
# Without this a program built with MinGW does not start on a machine that lacks those three DLLs
# next to it ("libstdc++-6.dll was not found"); MSVC has no such problem, as it uses the system's
# runtime. The flag reaches every program that links talcite.
if(MINGW)
    target_link_libraries(talcite PUBLIC -static)
endif()
