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
        src/vulkan/rt64_raytracing_vk.cpp
        src/vulkan/rt64_mesh_vk.cpp
        src/vulkan/rt64_descriptor_layout_vk.cpp
        src/vulkan/rt64_texture_vk.cpp
        src/vulkan/rt64_scene_vk.cpp
        src/vulkan/rt64_view_vk.cpp
        src/vulkan/rt64_shader_compiler_vk.cpp
        src/vulkan/rt64_shader_vk.cpp
        src/vulkan/rt64_rt_pipeline_vk.cpp
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
            ${CMAKE_BINARY_DIR}/generated
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
    # Test shaders go elsewhere: they must not appear in the reflection of
    # RT64's own shaders, or TestRayGen would join the RayTracing group.
    rt64_compile_shaders(rt64_test_shaders
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/shaders
        ${CMAKE_BINARY_DIR}/test_shaders)

    # Descriptor set layouts are generated from the compiled SPIR-V so they
    # cannot drift from what the shaders declare.
    set(RT64_BINDINGS_HEADER ${CMAKE_BINARY_DIR}/generated/rt64_shader_bindings.h)
    add_custom_command(
        OUTPUT ${RT64_BINDINGS_HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/reflect_bindings.py
                ${CMAKE_BINARY_DIR}/shaders ${RT64_BINDINGS_HEADER}
        DEPENDS rt64_shaders ${CMAKE_CURRENT_SOURCE_DIR}/tools/reflect_bindings.py
        COMMENT "Reflecting descriptor bindings from SPIR-V"
        VERBATIM)
    add_custom_target(rt64_bindings DEPENDS ${RT64_BINDINGS_HEADER})
    add_dependencies(rt64 rt64_bindings)
    target_include_directories(rt64 PUBLIC ${CMAKE_BINARY_DIR}/generated)
    message(STATUS "RT64 shaders: dxc at ${RT64_DXC_EXECUTABLE}")

    # The runtime compiler must use the SAME register shifts as the build-time
    # one: runtime hit groups share a descriptor set with build-time shaders,
    # so a different shift binds them to the wrong slots silently. Passing the
    # CMake values through as definitions keeps a single source of truth.
    target_compile_definitions(rt64 PRIVATE
        RT64_VK_U_SHIFT=${RT64_VK_U_SHIFT}
        RT64_VK_T_SHIFT=${RT64_VK_T_SHIFT}
        RT64_VK_B_SHIFT=${RT64_VK_B_SHIFT}
        RT64_VK_S_SHIFT=${RT64_VK_S_SHIFT})

    # DXC's headers, for the runtime compiler.
    get_filename_component(_dxc_root "${_dxc_bin_dir}/.." ABSOLUTE)
    if(EXISTS ${_dxc_root}/include/dxc/dxcapi.h)
        target_include_directories(rt64 SYSTEM PRIVATE ${_dxc_root}/include)
        message(STATUS "RT64 runtime compiler: dxc headers at ${_dxc_root}/include")
    else()
        message(FATAL_ERROR
            "dxc headers not found at ${_dxc_root}/include/dxc.\n"
            "The runtime shader compiler needs them. ./tools/get_dxc.sh "
            "installs headers alongside the binary.")
    endif()
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

# Phase 2 milestone: fullscreen triangle through RT64's own shaders.
add_executable(rt64_raster_test tests/raster_test.cpp)
target_include_directories(rt64_raster_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_include_directories(rt64_raster_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_raster_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)
if(RT64_BUILD_SHADERS)
    add_dependencies(rt64_raster_test rt64_shaders)
endif()

# Isolates whether a payload write survives IgnoreHit(). Needs a GPU.
add_executable(rt64_payload_test tests/payload_test.cpp)
target_include_directories(rt64_payload_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_include_directories(rt64_payload_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_payload_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)
if(RT64_BUILD_SHADERS)
    add_dependencies(rt64_payload_test rt64_test_shaders)
endif()

# The five ray passes, dispatched. Needs a GPU.
add_executable(rt64_dispatch_test tests/dispatch_test.cpp)
target_include_directories(rt64_dispatch_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan
    ${CMAKE_BINARY_DIR}/generated)
target_include_directories(rt64_dispatch_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_dispatch_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2 ${CMAKE_DL_LIBS})
add_dependencies(rt64_dispatch_test rt64_bindings)

# The real ray tracing pipeline: raygen + miss + per-material hit groups.
add_executable(rt64_pipeline_test tests/pipeline_test.cpp)
target_include_directories(rt64_pipeline_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan
    ${CMAKE_BINARY_DIR}/generated)
target_include_directories(rt64_pipeline_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_pipeline_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2 ${CMAKE_DL_LIBS})
add_dependencies(rt64_pipeline_test rt64_bindings)

# RT64_CreateShader against real combiner IDs. No GPU needed.
add_executable(rt64_shadergen_test tests/shadergen_test.cpp)
target_include_directories(rt64_shadergen_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_link_libraries(rt64_shadergen_test PRIVATE rt64 ${CMAKE_DL_LIBS})

# Runtime HLSL -> SPIR-V compilation. No GPU needed.
add_executable(rt64_compiler_test tests/compiler_test.cpp)
target_include_directories(rt64_compiler_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_link_libraries(rt64_compiler_test PRIVATE rt64 ${CMAKE_DL_LIBS})
target_include_directories(rt64_compiler_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# View render targets and the ray tracing descriptor set. Headless.
add_executable(rt64_view_test tests/view_test.cpp)
target_include_directories(rt64_view_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan
    ${CMAKE_BINARY_DIR}/generated)
target_include_directories(rt64_view_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_view_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)
add_dependencies(rt64_view_test rt64_bindings)

# Scenes, instances and TLAS building through the C ABI. Headless.
add_executable(rt64_scene_test tests/scene_test.cpp)
target_include_directories(rt64_scene_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_include_directories(rt64_scene_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_scene_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)

# Exercises RT64_CreateTexture through the C ABI. Headless.
add_executable(rt64_texture_test tests/texture_test.cpp)
target_include_directories(rt64_texture_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_include_directories(rt64_texture_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_texture_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)

# Creates every pass's descriptor set layout on the device. Headless.
add_executable(rt64_descriptor_test tests/descriptor_test.cpp)
target_include_directories(rt64_descriptor_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan
    ${CMAKE_BINARY_DIR}/generated)
target_include_directories(rt64_descriptor_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_descriptor_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)
add_dependencies(rt64_descriptor_test rt64_bindings)

# Exercises RT64_CreateMesh / RT64_SetMesh through the C ABI. Headless.
add_executable(rt64_mesh_test tests/mesh_test.cpp)
target_include_directories(rt64_mesh_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_include_directories(rt64_mesh_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_mesh_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)

# Phase 3 milestone: BLAS/TLAS, ray tracing pipeline, SBT, vkCmdTraceRaysKHR.
add_executable(rt64_raytrace_test tests/raytrace_test.cpp)
target_include_directories(rt64_raytrace_test PRIVATE
    ${RT64_PUBLIC_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan)
target_include_directories(rt64_raytrace_test SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/contrib/vma)
target_link_libraries(rt64_raytrace_test PRIVATE rt64 Vulkan::Vulkan PkgConfig::SDL2)
if(RT64_BUILD_SHADERS)
    add_dependencies(rt64_raytrace_test rt64_shaders rt64_test_shaders)
endif()
