cmake_minimum_required(VERSION 3.29)

project(WebbrowserInGamePlugin LANGUAGES CXX RC)

set(CEF_ROOT "" CACHE PATH "Path to the extracted CEF 152 Windows x64 package")

if(NOT CEF_ROOT OR NOT EXISTS "${CEF_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "Set CEF_ROOT to the extracted CEF 152 Windows x64 package directory, "
        "for example with -DCEF_ROOT=\"C:/deps/cef_binary_152..._windows64_minimal\".")
endif()

set(CMAKE_MODULE_PATH "${CEF_ROOT}/cmake" ${CMAKE_MODULE_PATH})
find_package(CEF REQUIRED)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/cef-wrapper")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/d2rl-ingame-webbrowser.toml"
    WEBBROWSER_DEFAULT_CONFIG)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/default_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/default_config.hpp"
    @ONLY
)

add_library(d2rl_ingame_webbrowser SHARED
    src/plugin.cpp
    src/plugin.rc
)
target_include_directories(d2rl_ingame_webbrowser PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/PluginSDK-0.4.0/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_features(d2rl_ingame_webbrowser PRIVATE cxx_std_20)
target_compile_definitions(d2rl_ingame_webbrowser PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_link_libraries(d2rl_ingame_webbrowser PRIVATE shlwapi shell32 user32)
set_target_properties(d2rl_ingame_webbrowser PROPERTIES
    OUTPUT_NAME "d2rl-ingame-webbrowser"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stage/plugin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stage/plugin"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols"
    COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols"
)
if(MSVC)
    target_compile_options(d2rl_ingame_webbrowser PRIVATE /EHsc /utf-8 /W4 /permissive-)
    target_link_options(d2rl_ingame_webbrowser PRIVATE /INCREMENTAL:NO /Brepro)
endif()
add_custom_command(TARGET d2rl_ingame_webbrowser POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/example.html"
        "$<TARGET_FILE_DIR:d2rl_ingame_webbrowser>/example.html"
    VERBATIM
)

set(CEF_STAGE_DIR "${CMAKE_BINARY_DIR}/stage/plugin/CEF corefiles")
add_library(cefsimple_client SHARED src/cef_client.cpp)
set_property(TARGET cefsimple_client PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(cefsimple_client PRIVATE "${CEF_ROOT}")
target_compile_features(cefsimple_client PRIVATE cxx_std_20)
target_compile_definitions(cefsimple_client PRIVATE
    CEF_USE_BOOTSTRAP
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_link_libraries(cefsimple_client PRIVATE
    libcef_dll_wrapper
    "${CEF_LIB_RELEASE}"
    ${CEF_STANDARD_LIBS}
)
set_target_properties(cefsimple_client PROPERTIES
    OUTPUT_NAME "cefsimple"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CEF_STAGE_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CEF_STAGE_DIR}"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols"
    COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/symbols"
)
if(MSVC)
    target_compile_options(cefsimple_client PRIVATE /EHsc /utf-8 /W4 /permissive-)
    target_link_options(cefsimple_client PRIVATE /INCREMENTAL:NO /Brepro)
endif()

set(CEF_RUNTIME_FILES
    chrome_elf.dll
    d3dcompiler_47.dll
    dxcompiler.dll
    dxil.dll
    libcef.dll
    v8_context_snapshot.bin
    vk_swiftshader.dll
    vk_swiftshader_icd.json
    vulkan-1.dll
)
set(CEF_RUNTIME_COPY_COMMANDS)
foreach(CEF_FILE IN LISTS CEF_RUNTIME_FILES)
    list(APPEND CEF_RUNTIME_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CEF_ROOT}/Release/${CEF_FILE}" "${CEF_STAGE_DIR}/${CEF_FILE}"
    )
endforeach()
add_custom_target(stage_cef_runtime
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CEF_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CEF_ROOT}/Release/bootstrap.exe" "${CEF_STAGE_DIR}/cefsimple.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CEF_ROOT}/Resources" "${CEF_STAGE_DIR}"
    ${CEF_RUNTIME_COPY_COMMANDS}
    DEPENDS cefsimple_client
    VERBATIM
)

add_custom_target(stage_all DEPENDS d2rl_ingame_webbrowser stage_cef_runtime)
