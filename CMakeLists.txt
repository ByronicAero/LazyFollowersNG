cmake_minimum_required(VERSION 3.24)

project(LazyFollowersNG VERSION 1.3.0 LANGUAGES CXX)  # -> LazyFollowersNG.dll

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "Build with MSVC (Visual Studio 2022 / 2026). Use build.bat or the 'release' preset.")
endif()

# ---------------------------------------------------------------------------
# CommonLibSSE-NG (one DLL for Skyrim SE 1.5.97 and AE 1.6.x)
# ---------------------------------------------------------------------------
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
    GIT_TAG        v3.7.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(CommonLibSSE)

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")

add_commonlibsse_plugin(
    ${PROJECT_NAME}
    NAME "Lazy Followers NG"
    AUTHOR "Byronic"
    VERSION 1.3.0
    USE_ADDRESS_LIBRARY
    SOURCES
        src/main.cpp
        src/Papyrus.cpp
        src/LF.cpp
        src/Files.cpp
        src/Tint.cpp
        src/Menu.cpp
        src/Hotkey.cpp
        src/Voice.cpp
)

target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE include src)
# after add_commonlibsse_plugin: FindZLIB leaves regex match variables that break its version parsing
find_package(ZLIB REQUIRED)  # compressed records in plugin files (Voice.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json ZLIB::ZLIB)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(${PROJECT_NAME} PRIVATE /permissive- /Zc:preprocessor /EHsc /utf-8 /W4 /wd4100)

# Assemble a ready-to-install mod folder in build/<preset>/package
set(PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_DIR}/SKSE/Plugins"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${PACKAGE_DIR}/SKSE/Plugins/"
    VERBATIM
)
# LazyFollowers.esp with every script (VMAD) stripped, 100 slots, ESL-flagged with renumbered IDs
# (Tools/eslify.py; old IDs -> new in src/EslIds.h). Same file name. No .pex files - the DLL does what the scripts did.
# Only the esp from data/ (its README.txt stays out of the zip); LazyGirl's esp, modified with her permission (credit).
file(GLOB LF_ESP "${CMAKE_SOURCE_DIR}/data/*.esp")
if(LF_ESP)
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${LF_ESP} "${PACKAGE_DIR}"
        VERBATIM
    )
else()
    message(WARNING "No esp in data/ - the zip will only have the DLL (see README.md, \"The esp\")")
endif()
