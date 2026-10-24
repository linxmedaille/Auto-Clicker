cmake_minimum_required(VERSION 3.21)

# Static CRT for MSVC (applies to webview too, so it must be set before fetching it).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(autoclicker VERSION 3.0.0 LANGUAGES C CXX RC)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- webview
# Static library only. Its built-in WebView2 loader means no WebView2Loader.dll
# has to ship next to the exe.
set(WEBVIEW_BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_USE_BUILTIN_MSWEBVIEW2 ON CACHE BOOL "" FORCE)
set(WEBVIEW_USE_STATIC_MSVC_RUNTIME ON CACHE BOOL "" FORCE)
set(WEBVIEW_USE_COMPAT_MINGW ${MINGW} CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_CHECKS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(webview
    URL https://github.com/webview/webview/archive/refs/tags/0.12.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(webview)

# ---------------------------------------------------------------- Embedded UI
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(UI_HEADER "${GENERATED_DIR}/ui_html.h")
add_custom_command(
    OUTPUT "${UI_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/ui.html"
            "-DFONTS=${CMAKE_CURRENT_SOURCE_DIR}/vendor/fonts.css"
            "-DGSAP=${CMAKE_CURRENT_SOURCE_DIR}/vendor/gsap.min.js"
            "-DOUTPUT=${UI_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
    DEPENDS ui.html vendor/fonts.css vendor/gsap.min.js cmake/embed.cmake
    COMMENT "Embedding ui.html with vendored fonts and GSAP"
    VERBATIM)

# ---------------------------------------------------------------- Executable
add_executable(autoclicker WIN32 main.c clicker.c app.rc "${UI_HEADER}")

set_source_files_properties(app.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")

target_include_directories(autoclicker PRIVATE "${GENERATED_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(autoclicker PRIVATE UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(autoclicker PRIVATE webview::core_static winmm comctl32)

# webview is C++, so let the C++ driver link (pulls in the C++ runtime).
set_target_properties(autoclicker PROPERTIES LINKER_LANGUAGE CXX)

if(MSVC)
    target_compile_options(autoclicker PRIVATE /W4 /utf-8)
    # The manifest comes from app.rc; stop the linker from generating a second one.
    target_link_options(autoclicker PRIVATE /MANIFEST:NO)
else()
    target_compile_options(autoclicker PRIVATE -Wall -Wextra)
    target_link_options(autoclicker PRIVATE -mwindows -static -static-libgcc -static-libstdc++)
endif()
