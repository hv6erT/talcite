# Fuzz targets for the readers of hostile text, for whoever works on them: libFuzzer drives them,
# AddressSanitizer and UBSan watch them. Everything in tal is instrumented, so that the fuzzer sees
# the coverage of all the code a text reaches. Clang only, and a build directory of its own.
option(TAL_FUZZ "Build the fuzz targets of tal (Clang only; instruments the whole build)" OFF)

if(TAL_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "TAL_FUZZ needs Clang, for libFuzzer.")
  endif()

  add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# tal::core of talcite.next: tasks and their owners, executors, the clock, troubles, ports, the
# log, paths, the files port with its memory adapter, network shares, streams, the processes port
# with its scripted adapter, JSON, XML, archives (zip and tar, plain or through gzip, with our own
# inflate), text (UTF-8, terminal cells, letter case, collation and the rules of
# languages), the dictionary of a program's texts, its settings, and in platform/ the system's adapters of the ports, built in as their defaults. It
# depends on nothing but the standard library and the system's own libraries.
find_package(Threads REQUIRED)

add_library(tal-core STATIC src/archive.cpp src/basics.cpp src/crypto.cpp src/executor.cpp src/deflate.cpp src/fs.cpp src/inflate.cpp src/memory.cpp src/notify.cpp src/path.cpp src/process.cpp src/share.cpp src/stream.cpp src/sums.cpp src/tar.cpp src/task.cpp
  src/collate.cpp src/json.cpp src/language.cpp src/mending.cpp src/settings.cpp src/text.cpp src/texts.cpp src/unicode.cpp src/wording.cpp src/xml.cpp src/zip.cpp
  src/platform/bell.cpp src/platform/child_core.cpp src/platform/cipher_core.cpp src/platform/crypto_core.cpp src/platform/copy_core.cpp src/platform/disk.cpp src/platform/disk_core.cpp src/platform/file_core.cpp
  src/platform/language_core.cpp
  src/platform/trash_core.cpp src/platform/watch_core.cpp src/simulation.cpp)
target_include_directories(tal-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tal-core PUBLIC Threads::Threads)
target_compile_features(tal-core PUBLIC cxx_std_20)

# Windows: the network connections of mapped drives and shares (mpr), the shares of a server
# (netapi32), the shell's known folders, recycle bin and notification area (shell32, ole32), the
# window that hears the notification area (user32), the rights of files and the stored logins
# (advapi32) and the digests and randomness of CNG (bcrypt).
if(WIN32)
  target_link_libraries(tal-core PUBLIC mpr netapi32 shell32 ole32 user32 advapi32 bcrypt)
endif()

# What weakens security is built in only on the programmer's word: our own digests as the spare
# where the system gives none (TAL_OWN_SUMS), MD5 and SHA-1, broken for collisions
# (TAL_WEAK_SUMS), and our own arithmetic of curve 25519, which CPace needs and no system gives
# (TAL_OWN_CURVE), and the logins of shares kept by Windows's Credential Manager
# (TAL_REMEMBER_LOGINS). Without them that code is not in the program.
option(TAL_OWN_SUMS "Our own digests where the system gives none" OFF)
option(TAL_WEAK_SUMS "The digests MD5 and SHA-1" OFF)
option(TAL_OWN_CURVE "Our own arithmetic of curve 25519, for CPace" OFF)
option(TAL_REMEMBER_LOGINS "Logins of shares remembered by the system (Windows)" OFF)

if(TAL_OWN_SUMS)
  target_compile_definitions(tal-core PUBLIC TAL_OWN_SUMS)
endif()

if(TAL_WEAK_SUMS)
  target_compile_definitions(tal-core PUBLIC TAL_WEAK_SUMS)
endif()

if(TAL_REMEMBER_LOGINS)
  target_compile_definitions(tal-core PUBLIC TAL_REMEMBER_LOGINS)
endif()

# tal-texts, the generator of a program's texts, run while the program is built: out of the
# English texts it makes C++ functions, one for each text, and it checks every other language
# against English. A cross build cannot run what it builds, so there TAL_TEXTS names tal-texts
# built for the host.
add_executable(tal-texts tools/texts.cpp tools/files.cpp tools/generating.cpp)
target_include_directories(tal-texts PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tal-texts PRIVATE tal-core)

set(TAL_TEXTS "" CACHE FILEPATH "tal-texts built for the host, for a cross build")

# tal-settings, the generator of a program's settings, run while the program is built: out of the
# built-in values it makes a handle with its type for each setting, and the function that gives
# the values. In a cross build TAL_SETTINGS names it built for the host.
add_executable(tal-settings tools/settings.cpp tools/files.cpp tools/generating.cpp)
target_include_directories(tal-settings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tal-settings PRIVATE tal-core)

set(TAL_SETTINGS "" CACHE FILEPATH "tal-settings built for the host, for a cross build")

# tal_texts(<target> <en-US.json> <namespace>): the texts of <target>. The functions go into
# <namespace>.hpp and .cpp in the build directory, made again when English changes; every language
# beside English is checked whenever one of them changes, and a problem stops the build with its
# file, line and column.
function(tal_texts target english space)
  get_filename_component(english "${english}" ABSOLUTE)
  get_filename_component(folder "${english}" DIRECTORY)
  string(REPLACE "::" ";" parts "${space}")
  list(GET parts -1 name)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/tal-texts/${target}")

  if(CMAKE_CROSSCOMPILING)
    if(NOT TAL_TEXTS)
      message(FATAL_ERROR "A cross build runs the generator of texts on the host: set TAL_TEXTS to tal-texts built for it.")
    endif()

    set(generator "${TAL_TEXTS}")
  else()
    set(generator tal-texts)
  endif()

  file(GLOB languages CONFIGURE_DEPENDS "${folder}/*.json")

  add_custom_command(OUTPUT "${out}/${name}.hpp" "${out}/${name}.cpp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
    COMMAND ${generator} generate "${english}" "${space}" "${out}/${name}.hpp" "${out}/${name}.cpp"
    DEPENDS "${english}" ${generator}
    COMMENT "Making the texts of ${target}"
    VERBATIM)

  add_custom_command(OUTPUT "${out}/checked"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
    COMMAND ${generator} check "${out}/checked" "${english}" ${languages}
    DEPENDS ${languages} ${generator}
    COMMENT "Checking the languages of ${target}"
    VERBATIM)

  target_sources(${target} PRIVATE "${out}/${name}.hpp" "${out}/${name}.cpp" "${out}/checked")
  target_include_directories(${target} PUBLIC "${out}")
  target_link_libraries(${target} PUBLIC tal-core)
endfunction()

# tal_settings(<target> <defaults.json> <namespace>): the settings of <target>. The handles and
# the built-in values go into <namespace>.hpp and .cpp in the build directory, made again when the
# built-in values change; a problem stops the build with its file, line and column.
function(tal_settings target defaults space)
  get_filename_component(defaults "${defaults}" ABSOLUTE)
  string(REPLACE "::" ";" parts "${space}")
  list(GET parts -1 name)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/tal-settings/${target}")

  if(CMAKE_CROSSCOMPILING)
    if(NOT TAL_SETTINGS)
      message(FATAL_ERROR "A cross build runs the generator of settings on the host: set TAL_SETTINGS to tal-settings built for it.")
    endif()

    set(generator "${TAL_SETTINGS}")
  else()
    set(generator tal-settings)
  endif()

  add_custom_command(OUTPUT "${out}/${name}.hpp" "${out}/${name}.cpp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
    COMMAND ${generator} generate "${defaults}" "${space}" "${out}/${name}.hpp" "${out}/${name}.cpp"
    DEPENDS "${defaults}" ${generator}
    COMMENT "Making the settings of ${target}"
    VERBATIM)

  target_sources(${target} PRIVATE "${out}/${name}.hpp" "${out}/${name}.cpp")
  target_include_directories(${target} PUBLIC "${out}")
  target_link_libraries(${target} PUBLIC tal-core)
endfunction()

# tal_embedded(<target> <namespace> <file>...): files carried inside <target>, each a constant
# std::string_view named after the file in <namespace>.hpp and .cpp in the build directory, made
# again when a file changes. A program that is copied elsewhere, as the trials are to Windows,
# keeps them.
function(tal_embedded target space)
  set(files "")

  foreach(file IN LISTS ARGN)
    get_filename_component(file "${file}" ABSOLUTE)
    list(APPEND files "${file}")
  endforeach()

  string(REPLACE "::" ";" parts "${space}")
  list(GET parts -1 name)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/tal-embedded/${target}")
  string(REPLACE ";" "|" joined "${files}")

  add_custom_command(OUTPUT "${out}/${name}.hpp" "${out}/${name}.cpp"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}"
    COMMAND "${CMAKE_COMMAND}" "-DSPACE=${space}" "-DHEADER=${out}/${name}.hpp" "-DSOURCE=${out}/${name}.cpp"
            "-DFILES=${joined}" -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tools/embedded.cmake"
    DEPENDS ${files} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tools/embedded.cmake"
    COMMENT "Embedding the files of ${target}"
    VERBATIM)

  target_sources(${target} PRIVATE "${out}/${name}.hpp" "${out}/${name}.cpp")
  target_include_directories(${target} PRIVATE "${out}")
endfunction()

# tal::media of talcite.next: images as RGBA frames, their format and size read from the header,
# scaling in linear light, and decoding frame by frame through the port `Decoding`, whose default
# is a chain of our own PNG, JPEG and GIF; and the metadata of sound and video files, the tags and
# the properties (tal::meta), and the structure of PDF documents, read by our own readers.
# Documents and pages rendered by another process (tal::docs): the ports `Renderer` and
# `Browser`, their chains, and their adapters on any program that speaks tal's protocol. Drawing
# (tal::drawing): paths, strokes, paints and clips rasterized into pixels by our own rasterizer,
# and text from fonts read by our own reader of TrueType; DejaVu Sans built in (fonts/, its own
# object, linked only into programs that use it).
add_library(tal-media STATIC src/pixels.cpp src/gif.cpp src/png.cpp src/jpeg.cpp src/preview.cpp src/thumbnail.cpp src/meta.cpp src/walk.cpp src/id3.cpp src/ape.cpp src/mpeg.cpp src/flac.cpp src/ogg.cpp src/wavpack.cpp src/mp4.cpp src/matroska.cpp src/avi.cpp src/pdfsyntax.cpp src/pdf.cpp src/docs.cpp src/charsets.cpp src/htmltext.cpp src/pdftext.cpp src/rendering.cpp src/exif.cpp src/xmp.cpp src/iptc.cpp src/photo.cpp src/photonames.cpp src/exifwrite.cpp src/paths.cpp src/flattening.cpp src/stroking.cpp src/rasterizing.cpp src/raster.cpp src/fonts.cpp src/dejavu.cpp)
target_link_libraries(tal-media PUBLIC tal-core)
tal_embedded(tal-media fonts_of fonts/DejaVuSans.ttf)

# tal::plotting, a module of its own over the drawing of tal-media: scales, ticks and their labels,
# the layout of a plot, its series as lines, marks, bars and steps drawn with the pen on anything
# that has one, histograms of values, the readout under the cursor, and traces of samples in time.
add_library(tal-plotting STATIC src/plotting.cpp)
target_link_libraries(tal-plotting PUBLIC tal-media)

# PDF rendered by PDFium (tal-render-pdfium), only on the programmer's word: configuring it fetches
# PDFium, the build of bblanchon/pdfium-binaries without V8 (so no JavaScript or XFA in PDF), and
# checks it by its SHA-256. The pinned build has the sums of Linux and Windows on x64; for any other
# system, or a build without the network, the programmer gives the archive (TAL_PDFIUM_ARCHIVE) and,
# for another one than pinned, its sum (TAL_PDFIUM_SHA256). The program and the library land
# beside the programs of the build, the licenses of PDFium and what it holds in licenses/pdfium.
option(TAL_PDFIUM "Render PDF through PDFium in a process of its own (fetches PDFium)" OFF)
set(TAL_PDFIUM_ARCHIVE "" CACHE FILEPATH "PDFium's archive, fetched already (pdfium-<system>.tgz)")
set(TAL_PDFIUM_SHA256 "" CACHE STRING "The SHA-256 of PDFium's archive for another system than pinned")

if(TAL_PDFIUM)
  set(pdfium_release "chromium/8165")

  if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|arm|aarch64")
    set(pdfium_system "win-x64")
    set(pdfium_sum "ca7338ae7779b8b1b82f4580305c4d762d214f8df1d54b47647fe3cc75989ded")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(pdfium_system "linux-x64")
    set(pdfium_sum "ba7865e16c0271daab75256790be06c26272190afe6e2a70eae0bb39bb99089c")
  else()
    set(pdfium_system "")
    set(pdfium_sum "")
  endif()

  if(TAL_PDFIUM_SHA256)
    set(pdfium_sum "${TAL_PDFIUM_SHA256}")
  endif()

  if(NOT pdfium_sum)
    message(FATAL_ERROR "TAL_PDFIUM: no build of PDFium is pinned for this system; give TAL_PDFIUM_ARCHIVE and TAL_PDFIUM_SHA256.")
  endif()

  string(TOLOWER "${pdfium_sum}" pdfium_sum)
  set(pdfium_home "${CMAKE_CURRENT_BINARY_DIR}/pdfium")

  if(TAL_PDFIUM_ARCHIVE)
    set(pdfium_archive "${TAL_PDFIUM_ARCHIVE}")
  else()
    if(NOT pdfium_system)
      message(FATAL_ERROR "TAL_PDFIUM: give TAL_PDFIUM_ARCHIVE for this system.")
    endif()

    set(pdfium_archive "${CMAKE_CURRENT_BINARY_DIR}/pdfium-${pdfium_system}.tgz")
    file(DOWNLOAD "https://github.com/bblanchon/pdfium-binaries/releases/download/${pdfium_release}/pdfium-${pdfium_system}.tgz" "${pdfium_archive}"
         EXPECTED_HASH SHA256=${pdfium_sum} TLS_VERIFY ON STATUS pdfium_fetched)
    list(GET pdfium_fetched 0 pdfium_failed)

    if(pdfium_failed)
      list(GET pdfium_fetched 1 pdfium_said)
      message(FATAL_ERROR "TAL_PDFIUM: PDFium could not be fetched: ${pdfium_said}")
    endif()
  endif()

  file(SHA256 "${pdfium_archive}" pdfium_found)

  if(NOT pdfium_found STREQUAL pdfium_sum)
    message(FATAL_ERROR "TAL_PDFIUM: ${pdfium_archive} has the SHA-256 ${pdfium_found}, not ${pdfium_sum}.")
  endif()

  if(NOT EXISTS "${pdfium_home}/${pdfium_found}")
    file(REMOVE_RECURSE "${pdfium_home}")
    file(ARCHIVE_EXTRACT INPUT "${pdfium_archive}" DESTINATION "${pdfium_home}")
    file(TOUCH "${pdfium_home}/${pdfium_found}")
  endif()

  file(STRINGS "${pdfium_home}/VERSION" pdfium_parts)
  set(pdfium_version "")

  foreach(part IN LISTS pdfium_parts)
    string(REGEX REPLACE "^[A-Z]+=" "" number "${part}")
    string(APPEND pdfium_version ".${number}")
  endforeach()

  string(SUBSTRING "${pdfium_version}" 1 -1 pdfium_version)

  add_library(tal-pdfium SHARED IMPORTED)
  set_target_properties(tal-pdfium PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${pdfium_home}/include")

  if(WIN32)
    set(pdfium_library "${pdfium_home}/bin/pdfium.dll")

    if(MSVC)
      set_target_properties(tal-pdfium PROPERTIES IMPORTED_LOCATION "${pdfium_library}" IMPORTED_IMPLIB "${pdfium_home}/lib/pdfium.dll.lib")
    else()
      set_target_properties(tal-pdfium PROPERTIES IMPORTED_LOCATION "${pdfium_library}" IMPORTED_IMPLIB "${pdfium_library}")
    endif()
  else()
    set(pdfium_library "${pdfium_home}/lib/libpdfium${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set_target_properties(tal-pdfium PROPERTIES IMPORTED_LOCATION "${pdfium_library}" IMPORTED_NO_SONAME TRUE)
  endif()

  add_executable(tal-render-pdfium engines/pdfium.cpp)
  target_link_libraries(tal-render-pdfium PRIVATE tal-media tal-pdfium)
  target_include_directories(tal-render-pdfium PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(tal-render-pdfium PRIVATE TAL_PDFIUM_VERSION="${pdfium_version}")
  set_target_properties(tal-render-pdfium PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
  add_custom_command(TARGET tal-render-pdfium POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different "${pdfium_library}" "$<TARGET_FILE_DIR:tal-render-pdfium>"
                     COMMAND ${CMAKE_COMMAND} -E copy_directory "${pdfium_home}/licenses" "$<TARGET_FILE_DIR:tal-render-pdfium>/licenses/pdfium"
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different "${pdfium_home}/LICENSE" "$<TARGET_FILE_DIR:tal-render-pdfium>/licenses/pdfium/LICENSE")
  target_compile_definitions(tal-media PRIVATE TAL_PDFIUM)
endif()

# Web pages shown by WebKitGTK 6 (tal-render-gtk) on Linux, only on the programmer's word: it
# needs webkitgtk-6.0 through pkg-config at build time, and gtk4-broadwayd when it runs.
option(TAL_WEBKITGTK "Show web pages through WebKitGTK 6 in a process of its own (Linux)" OFF)

if(TAL_WEBKITGTK)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(TAL_WEBKIT REQUIRED IMPORTED_TARGET webkitgtk-6.0 gtk4)
  add_executable(tal-render-gtk engines/gtk.cpp)
  target_link_libraries(tal-render-gtk PRIVATE tal-media PkgConfig::TAL_WEBKIT)
  target_include_directories(tal-render-gtk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  tal_embedded(tal-render-gtk helper_of engines/helper.js)
  target_compile_definitions(tal-media PRIVATE TAL_WEBKITGTK)
endif()

# Web pages shown by WebView2 (tal-render-webview2) on Windows, only on the programmer's word. Its
# SDK is the NuGet package Microsoft.Web.WebView2 1.0.4314.35: given unpacked (TAL_WEBVIEW2_SDK),
# or fetched from NuGet and checked against its SHA-256 (TAL_WEBVIEW2_SHA256, pinned here as
# certutil -hashfile <package> SHA256 told it). WebView2Loader.dll lands beside the program, and
# MinGW gets the header EventToken.h through a shim.
option(TAL_WEBVIEW2 "Show web pages through WebView2 in a process of its own (Windows)" OFF)
set(TAL_WEBVIEW2_SDK "" CACHE PATH "The NuGet package of WebView2, unpacked")
set(TAL_WEBVIEW2_SHA256 "90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4" CACHE STRING "The SHA-256 of the NuGet package of WebView2 1.0.4314.35")

if(TAL_WEBVIEW2)
  if(NOT WIN32)
    message(FATAL_ERROR "TAL_WEBVIEW2: WebView2 is for Windows.")
  endif()

  if(TAL_WEBVIEW2_SDK)
    set(webview2_home "${TAL_WEBVIEW2_SDK}")
  else()
    if(NOT TAL_WEBVIEW2_SHA256)
      message(FATAL_ERROR "TAL_WEBVIEW2: give TAL_WEBVIEW2_SDK, or TAL_WEBVIEW2_SHA256 to fetch the package from NuGet.")
    endif()

    set(webview2_package "${CMAKE_CURRENT_BINARY_DIR}/webview2.nupkg")
    file(DOWNLOAD "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.4314.35" "${webview2_package}"
         EXPECTED_HASH "SHA256=${TAL_WEBVIEW2_SHA256}" STATUS webview2_status)
    list(GET webview2_status 0 webview2_code)

    if(NOT webview2_code EQUAL 0)
      message(FATAL_ERROR "TAL_WEBVIEW2: the package could not be fetched: ${webview2_status}")
    endif()

    set(webview2_home "${CMAKE_CURRENT_BINARY_DIR}/webview2")
    file(ARCHIVE_EXTRACT INPUT "${webview2_package}" DESTINATION "${webview2_home}")
  endif()

  add_executable(tal-render-webview2 engines/webview2.cpp)
  target_link_libraries(tal-render-webview2 PRIVATE tal-media "${webview2_home}/build/native/x64/WebView2Loader.dll.lib" ole32 shlwapi user32)
  target_include_directories(tal-render-webview2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${webview2_home}/build/native/include")

  if(NOT MSVC)
    target_include_directories(tal-render-webview2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engines/shim")
  endif()

  tal_embedded(tal-render-webview2 helper_of engines/helper.js)
  add_custom_command(TARGET tal-render-webview2 POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different "${webview2_home}/build/native/x64/WebView2Loader.dll" "$<TARGET_FILE_DIR:tal-render-webview2>"
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different "${webview2_home}/LICENSE.txt" "$<TARGET_FILE_DIR:tal-render-webview2>/licenses/webview2/LICENSE.txt")
  target_compile_definitions(tal-media PRIVATE TAL_WEBVIEW2)
endif()

# tal::ui of talcite.next: the geometry of cells, looks and style sheets with their selectors
# and the built-in sheet, the surface of cells a frame is painted on, keys and events, and the
# tree of nodes with its document, which delivers events, keeps the focus and the timers, and
# styles, lays out and paints what changed. Pictures come from tal-media: drawn in characters, or
# sent to the terminal as sixel or kitty pixels.
add_library(tal-ui STATIC src/action.cpp src/app.cpp src/capabilities.cpp src/clipboard.cpp src/conhost.cpp src/locating.cpp src/telling.cpp src/console.cpp src/damage.cpp src/decoder.cpp src/diff.cpp src/document.cpp src/emulator.cpp src/terminal.cpp src/editing.cpp src/event.cpp src/fields.cpp
  src/controls.cpp src/geometry.cpp src/key.cpp src/layout.cpp src/menu.cpp src/model.cpp src/pages.cpp src/editor.cpp src/lining.cpp src/dropping.cpp src/spooling.cpp src/image.cpp src/drawn.cpp src/leaves.cpp src/previewed.cpp src/webview.cpp src/written.cpp src/mosaic.cpp src/node.cpp src/sixel.cpp src/style.cpp src/surface.cpp src/undo.cpp src/views.cpp src/vt.cpp src/window.cpp
  src/platform/console_core.cpp)
target_link_libraries(tal-ui PUBLIC tal-media)

# Windows: the double-click time (user32) and NormalizeString (normaliz), which composes a letter
# with its marks for the console without VT, whose cell holds one UTF-16 unit.
if(WIN32)
  target_link_libraries(tal-ui PUBLIC user32 normaliz)
endif()

# The program that shows a simulation (E7e), among the demos: a double pendulum drawn in the
# terminal, its bodies dragged with the mouse, beside plots of its energy, angles and phase (E8),
# read with the mouse (E9), the phase or a histogram of the second angle (E10), its settings out of
# demo/pendulum.json.
if(TALCITE_DEMOS)
  add_executable(tal-pendulum demo/pendulum.cpp)
  target_link_libraries(tal-pendulum PRIVATE tal-ui tal-plotting)
  tal_settings(tal-pendulum demo/pendulum.json pendulum_settings)
endif()

# tal::net of talcite.next: addresses and names, TCP as a stream, listeners and UDP, waited for by
# one reactor thread for the whole program: epoll on Linux, kqueue on macOS and the BSDs, IOCP on
# Windows, poll elsewhere. TAL_NET_KQUEUE takes kqueue where the system has another, as Linux has
# through libkqueue, so that its backend is tried there too. TLS over any stream, an HTTP client with
# the proxy of the environment or, on Windows, of WinHTTP's setting, and what connects users: STUN,
# a beacon in the local network, ports opened on the router, and key agreements with the channel
# of sealed messages they open.
option(TAL_NET_KQUEUE "Wait for sockets through kqueue (libkqueue on Linux)" OFF)

add_library(tal-net STATIC src/net.cpp src/tls.cpp src/http.cpp src/stun.cpp src/beacon.cpp src/router.cpp src/handshake.cpp src/cpace.cpp src/curving.cpp src/sealed.cpp src/platform/iocp_core.cpp src/platform/reactor_core.cpp src/platform/sockets_core.cpp src/platform/tls_core.cpp src/platform/proxy_core.cpp)
target_link_libraries(tal-net PUBLIC tal-core)

if(TAL_OWN_CURVE)
  target_compile_definitions(tal-net PUBLIC TAL_OWN_CURVE)
endif()

if(WIN32)
  target_link_libraries(tal-net PUBLIC ws2_32 mswsock secur32 crypt32 ncrypt winhttp iphlpapi)
endif()

if(TAL_NET_KQUEUE)
  target_compile_definitions(tal-net PUBLIC TAL_NET_KQUEUE)

  if(NOT CMAKE_SYSTEM_NAME MATCHES "BSD|Darwin")
    find_path(TAL_KQUEUE_INCLUDE sys/event.h PATH_SUFFIXES kqueue REQUIRED)
    find_library(TAL_KQUEUE_LIBRARY kqueue REQUIRED)
    target_include_directories(tal-net PRIVATE "${TAL_KQUEUE_INCLUDE}")
    target_link_libraries(tal-net PUBLIC "${TAL_KQUEUE_LIBRARY}")
  endif()
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
  add_executable(tal-test ${TAL_TEST_SOURCES} tools/generating.cpp)
  target_link_libraries(tal-test PRIVATE tal-testing tal-scene tal-net tal-media tal-plotting)
  # Some trials check internal parts (the console translation without VT, the generator of texts)
  # through their headers.
  target_include_directories(tal-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tools")
  # The texts of the trials, made and checked as a program's are.
  tal_texts(tal-test test/texts/en-US.json spoken)
  tal_settings(tal-test test/settings/defaults.json preset)
  # The files of those texts travel inside the binary, which the trials on Windows run far from
  # the sources.
  tal_embedded(tal-test texts_of test/texts/en-US.json test/texts/pl-PL.json)
  # The documents of the trials of the text version of PDF and the texts they are to give.
  tal_embedded(tal-test documents_of test/documents/lmodern.pdf test/documents/ot1.pdf test/documents/bitmap.pdf)
  tal_embedded(tal-test expected_of test/documents/lmodern.txt test/documents/ot1.txt test/documents/bitmap.txt)

  if(TAL_PDFIUM)
    # The trials of PDFium run tal-render-pdfium beside them.
    add_dependencies(tal-test tal-render-pdfium)
    target_compile_definitions(tal-test PRIVATE TAL_PDFIUM)
  endif()

  if(TAL_WEBKITGTK)
    add_dependencies(tal-test tal-render-gtk)
    target_compile_definitions(tal-test PRIVATE TAL_WEBKITGTK)
  endif()

  if(TAL_WEBVIEW2)
    add_dependencies(tal-test tal-render-webview2)
    target_compile_definitions(tal-test PRIVATE TAL_WEBVIEW2)
  endif()

  add_test(NAME tal COMMAND tal-test)
endif()

if(TAL_FUZZ)
  add_subdirectory(fuzz)
endif()
