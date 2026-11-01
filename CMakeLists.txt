# VitoolBox.exe, its core library and the tests. Use build.ps1, which also
# fetches the dependencies and builds the installer.
#   -DVITOOL_WEBVIEW2_SDK=<Microsoft.Web.WebView2 package folder>
cmake_minimum_required(VERSION 3.21)
project(VitoolBox CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no Visual C++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/W4 /WX /permissive- /sdl /utf-8 /guard:cf /external:anglebrackets /external:W0)
add_link_options(/guard:cf /CETCOMPAT /OPT:REF /OPT:ICF)

add_library(vitoolcore STATIC
    src/core/backup.cpp
    src/core/download.cpp
    src/core/json.cpp
    src/core/packages.cpp
    src/core/paths.cpp
    src/core/processes.cpp
    src/core/startup.cpp
    src/core/text.cpp
)
target_include_directories(vitoolcore PUBLIC src)
target_link_libraries(vitoolcore PUBLIC advapi32 shell32 ole32 winhttp bcrypt)

add_executable(VitoolBox WIN32
    src/app/hub_window.cpp
    src/app/main.cpp
    src/app/update_window.cpp
    src/app/VitoolBox.rc
)
target_include_directories(VitoolBox SYSTEM PRIVATE ${VITOOL_WEBVIEW2_SDK}/build/native/include)
target_link_libraries(VitoolBox PRIVATE vitoolcore user32 gdi32 gdiplus dwmapi version uuid
    ${VITOOL_WEBVIEW2_SDK}/build/native/x64/WebView2LoaderStatic.lib)
# The manifest is embedded through VitoolBox.rc.
target_link_options(VitoolBox PRIVATE /MANIFEST:NO)
# The resource compiler's includes are not tracked: rebuild the version info
# and icon when they change.
set_source_files_properties(src/app/VitoolBox.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/version.h;${CMAKE_SOURCE_DIR}/src/app/VitoolBox.manifest;${CMAKE_SOURCE_DIR}/assets/vitoolbox.ico")

# The web UI is served from a "ui" folder next to the executable. Copied on
# every build: a post-build step would only run when the exe is relinked.
add_custom_target(VitoolBoxUi ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/ui
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/src/ui ${CMAKE_BINARY_DIR}/ui)
add_dependencies(VitoolBox VitoolBoxUi)

add_executable(VitoolBoxTests tests/tests.cpp)
target_link_libraries(VitoolBoxTests PRIVATE vitoolcore)
