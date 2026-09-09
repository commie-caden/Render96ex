cmake_minimum_required(VERSION 3.16)
project(rt64 LANGUAGES CXX)

# The original is a Visual Studio solution targeting D3D12. This replaces it.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(RT64_BUILD_STUB "Build the ABI stub instead of the real backend" ON)
option(RT64_BUILD_SHADERS "Compile the HLSL shaders to SPIR-V" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(Vulkan REQUIRED)

# SDL2 provides the window handle and, critically, tells us which surface
# extension to request at runtime — the session may be Wayland, X11, or
# XWayland, and only SDL knows which it chose.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

# ---------------------------------------------------------------- sources
set(RT64_SOURCES)

if(RT64_BUILD_STUB)
    # Phase 1 milestone: exports the full C ABI so the game can load and run
    # against it, every entry point failing cleanly. The Vulkan backend fills
    # in behind this same ABI.
    list(APPEND RT64_SOURCES
        src/vulkan/rt64_stub.cpp
        src/vulkan/rt64_device_vk.cpp
        src/vulkan/rt64_swapchain_vk.cpp
        src/vulkan/rt64_shaders_vk.cpp
        src/vulkan/rt64_vma_impl.cpp
    )
else()
    list(APPEND RT64_SOURCES
        src/rt64_common.cpp
        src/rt64_instance.cpp
        src/rt64_scene.cpp
        src/rt64_mesh.cpp
        src/rt64_texture.cpp
        src/rt64_shader.cpp
        src/rt64_device.cpp
        src/rt64_view.cpp
        src/rt64_mipmaps.cpp
        src/rt64_inspector.cpp
    )
endif()

# Fail early and legibly on an incomplete extraction. A missing source
# otherwise surfaces as "Cannot find source file" followed by a confusing
# "No SOURCES given to target", which looks like a broken CMakeLists rather
# than a broken unpack.
set(RT64_MISSING)
foreach(f ${RT64_SOURCES})
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
        list(APPEND RT64_MISSING ${f})
    endif()
endforeach()
if(RT64_MISSING)
    string(REPLACE ";" "\n    " RT64_MISSING_PRETTY "${RT64_MISSING}")
    message(FATAL_ERROR
        "Incomplete extraction — these source files are missing:\n"
        "    ${RT64_MISSING_PRETTY}\n\n"
        "Re-extract the archive over the game source root, then run\n"
        "    ./verify_install.sh\n"
        "to check every shipped file against MANIFEST.sha256.")
endif()

add_library(rt64 SHARED ${RT64_SOURCES})

# The public header is the single copy shared with the game tree. When rt64-vk
# lives inside the game source root, ../include holds it; standalone checkouts
# fall back to a local include/.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../include/rt64/rt64.h)
    set(RT64_PUBLIC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
else()
    set(RT64_PUBLIC_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
message(STATUS "RT64 public header from: ${RT64_PUBLIC_INCLUDE}")

target_include_directories(rt64
    PUBLIC  ${RT64_PUBLIC_INCLUDE}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan
)
target_include_directories(rt64 SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma
)

target_link_libraries(rt64 PRIVATE Vulkan::Vulkan PkgConfig::SDL2 ${CMAKE_DL_LIBS})

# Vulkan 1.4 target. Ray tracing stays an extension even in 1.4, so the
# extension checks in the device code are still required.
# Do NOT define VK_NO_PROTOTYPES here, not even to 0: vulkan.h tests it with
# #ifdef, so any definition removes the function prototypes and the link fails.
# RT64_VULKAN_API_VERSION is derived from the Vulkan headers in
# rt64_device_vk.h rather than forced here, so older SDKs still build.

target_compile_options(rt64 PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(src/vulkan/rt64_vma_impl.cpp PROPERTIES
    COMPILE_OPTIONS "-w")

# Produce librt64.so exactly as the loader in rt64.h expects.
set_target_properties(rt64 PROPERTIES
    OUTPUT_NAME rt64
    PREFIX "lib"
)

# ---------------------------------------------------------------- shaders
if(RT64_BUILD_SHADERS)
    include(CompileShaders)
    rt64_find_dxc()
    # dxc needs its own libdxcompiler.so; the bundled build is not on the
    # system library path.
    get_filename_component(_dxc_bin_dir "${RT64_DXC_EXECUTABLE}" DIRECTORY)
    if(_dxc_bin_dir)
        set(RT64_DXC_LIBRARY_DIR "${_dxc_bin_dir}/../lib")
    endif()
    rt64_compile_shaders(rt64_shaders
        ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders
        ${CMAKE_BINARY_DIR}/shaders)
    add_dependencies(rt64 rt64_shaders)
    message(STATUS "RT64 shaders: dxc at ${RT64_DXC_EXECUTABLE}")
endif()

# ------------------------------------------------------------------ tests
enable_testing()
add_executable(rt64_abi_test tests/abi_test.cpp)
target_include_directories(rt64_abi_test PRIVATE ${RT64_PUBLIC_INCLUDE})
target_link_libraries(rt64_abi_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(rt64_abi_test rt64)
add_test(NAME abi COMMAND rt64_abi_test $<TARGET_FILE:rt64>)

# Reports what the device selector actually picked. Not a test: it needs a GPU.
add_executable(rt64_device_info tests/device_info.cpp)
target_include_directories(rt64_device_info PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_include_directories(rt64_device_info SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_device_info PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)

# Phase 1 milestone: opens a window, clears it, presents. Needs a display, so
# it is not registered as a ctest.
add_executable(rt64_window_test tests/window_test.cpp)
target_include_directories(rt64_window_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_include_directories(rt64_window_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_window_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)

# Creates every shader module on the real device. Headless, needs a GPU.
add_executable(rt64_shader_test tests/shader_test.cpp)
target_include_directories(rt64_shader_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_include_directories(rt64_shader_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_shader_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)
if(RT64_BUILD_SHADERS)
    add_dependencies(rt64_shader_test rt64_shaders)
endif()
