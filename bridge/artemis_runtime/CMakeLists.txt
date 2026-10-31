cmake_minimum_required(VERSION 3.24)
project(aether_artemis_runtime LANGUAGES CXX)

# First implementation targets iOS only (native Metal).
if(NOT IOS)
    return()
endif()
set(ART3M1S_CORE_SOURCE "${CMAKE_SOURCE_DIR}/packages/Art3m1sCore")
if(NOT EXISTS "${ART3M1S_CORE_SOURCE}/Cargo.toml")
    message(FATAL_ERROR "Art3m1sCore checkout missing. Run git submodule update --init packages/Art3m1sCore")
endif()
find_program(ART3M1S_CARGO cargo REQUIRED)
if(CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        set(ART3M1S_TRIPLE "x86_64-apple-ios")
    else()
        set(ART3M1S_TRIPLE "aarch64-apple-ios-sim")
    endif()
else()
    set(ART3M1S_TRIPLE "aarch64-apple-ios")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ART3M1S_PROFILE "debug")
    set(ART3M1S_RELEASE_OPTION "")
else()
    set(ART3M1S_PROFILE "release")
    set(ART3M1S_RELEASE_OPTION "--release")
endif()
set(ART3M1S_TARGET_DIR "${CMAKE_BINARY_DIR}/art3m1s-target")
set(ART3M1S_ARCHIVE "${ART3M1S_TARGET_DIR}/${ART3M1S_TRIPLE}/${ART3M1S_PROFILE}/libaether_artemis_bridge.a")
add_custom_target(art3m1s_cargo_build ALL
    COMMAND "${CMAKE_COMMAND}" -E env
        "CARGO_TARGET_DIR=${ART3M1S_TARGET_DIR}"
        "${ART3M1S_CARGO}" build
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml"
            --target "${ART3M1S_TRIPLE}"
            ${ART3M1S_RELEASE_OPTION}
    BYPRODUCTS "${ART3M1S_ARCHIVE}"
    USES_TERMINAL
    VERBATIM)
add_library(aether_artemis_rust STATIC IMPORTED GLOBAL)
set_target_properties(aether_artemis_rust PROPERTIES IMPORTED_LOCATION "${ART3M1S_ARCHIVE}")
add_dependencies(aether_artemis_rust art3m1s_cargo_build)

add_library(aether_artemis_runtime STATIC src/artemis_runtime_provider.cpp)
target_compile_features(aether_artemis_runtime PRIVATE cxx_std_17)
target_include_directories(aether_artemis_runtime PRIVATE "${CMAKE_SOURCE_DIR}/abi/include")
target_link_libraries(aether_artemis_runtime PUBLIC engine_api aether_artemis_rust)
set_target_properties(aether_artemis_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(aether_artemis_runtime PUBLIC
    "-framework Metal" "-framework MetalKit" "-framework Foundation"
    "-framework CoreFoundation" "-framework CoreGraphics"
    "-framework CoreVideo" "-framework QuartzCore"
    "-framework Security" "-framework UIKit")
