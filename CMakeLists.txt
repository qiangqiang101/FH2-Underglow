cmake_minimum_required(VERSION 3.20)
project(Underglow VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    # The light code reads game memory inside __try/__except, which needs MSVC.
    message(FATAL_ERROR "Underglow builds with Visual Studio (MSVC) only.")
endif()

# The platform submodule, for its headers (fh2mod_api). The platform DLL itself isn't built here.
add_subdirectory(platform EXCLUDE_FROM_ALL)

add_library(Underglow SHARED src/neon_mod.cpp)
# src/fh2anna.h is FH2 ANNA's public header, copied from its repo (include/fh2anna.h).

target_link_libraries(Underglow PRIVATE fh2mod_api)
set_target_properties(Underglow PROPERTIES OUTPUT_NAME Underglow PREFIX "")
target_compile_definitions(Underglow PRIVATE
    UNDERGLOW_VERSION="${PROJECT_VERSION}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# Static runtime: avoid loading a second C++ runtime before Azimuth initializes another game.
set_property(TARGET Underglow PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(Underglow PRIVATE /W4 /permissive- /utf-8 /EHsc)
