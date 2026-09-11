# CompileShaders.cmake — HLSL to SPIR-V via DXC.
#
# The flags here are not arbitrary; both were established by testing the real
# shaders and both are load-bearing:
#
#   -HV 2018      HLSL 2021 forbids vector conditions in short-circuiting
#                 ternaries, which GenerateMipsCS.hlsl relies on.
#
#   -fvk-*-shift  DXC maps b/t/u/s registers into a single binding namespace
#                 per descriptor set, so b0, t0, u0 and s0 all collide on
#                 binding 0 (PrimaryRayGen alone aliases four resources).
#                 Shifting keeps UAVs at 0..31 in HeapIndices order and parks
#                 the rest clear of them.
#
#   -fvk-use-dx-layout
#                 D3D packs StructuredBuffer elements tightly; DXC's SPIR-V
#                 output otherwise uses a GLSL-like layout where float3 aligns
#                 to 16. LightInfo came out with diffuseColor at offset 16 and
#                 a stride of 64, against RT64_LIGHT's 12 and 60 — so every
#                 light field was misread and GetDimensions returned the wrong
#                 count. The same mismatch applies to instanceMaterials and
#                 instanceTransforms, which are also memcpy'd from C structs.
#
# Keep RT64_VK_*_SHIFT in step with the descriptor set layout in the backend:
# these numbers *are* the binding scheme.

set(RT64_VK_U_SHIFT 0)
set(RT64_VK_T_SHIFT 100)
set(RT64_VK_B_SHIFT 200)
set(RT64_VK_S_SHIFT 300)

# DXC's -fspv-target-env tops out at vulkan1.3. That is fine on a 1.4 device:
# it bounds the SPIR-V feature set, not the API being run against.
set(RT64_SPV_TARGET_ENV "vulkan1.3")

function(rt64_find_dxc)
    if(RT64_DXC_EXECUTABLE)
        return()
    endif()
    find_program(RT64_DXC_EXECUTABLE
        NAMES dxc
        HINTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dxc/bin
              $ENV{DXC_ROOT}/bin
        DOC "DirectX Shader Compiler with SPIR-V backend")

    if(NOT RT64_DXC_EXECUTABLE)
        message(FATAL_ERROR
            "dxc not found.\n\n"
            "DXC is not packaged for Fedora. Fetch the upstream Linux build:\n"
            "    ./tools/get_dxc.sh\n\n"
            "or point CMake at an existing one:\n"
            "    cmake -DRT64_DXC_EXECUTABLE=/path/to/dxc ...")
    endif()

    # A dxc built without the SPIR-V backend compiles nothing we need, and the
    # failure it produces otherwise is opaque.
    execute_process(COMMAND ${RT64_DXC_EXECUTABLE} --help
                    OUTPUT_VARIABLE dxc_help ERROR_QUIET)
    string(FIND "${dxc_help}" "-spirv" spirv_pos)
    if(spirv_pos EQUAL -1)
        message(FATAL_ERROR
            "${RT64_DXC_EXECUTABLE} was built without the SPIR-V backend.\n"
            "Use the upstream release build: ./tools/get_dxc.sh")
    endif()
    set(RT64_DXC_EXECUTABLE ${RT64_DXC_EXECUTABLE} PARENT_SCOPE)
endfunction()

# rt64_shader_profile(<name> <out_profile> <out_entry>)
# Entry point "-" means a raytracing library, which has no single entry.
function(rt64_shader_profile name out_profile out_entry)
    if(name MATCHES "^(ComposePS|DebugPS|Im3DPS|PostProcessPS)$")
        set(${out_profile} "ps_6_3" PARENT_SCOPE)
        set(${out_entry} "PSMain" PARENT_SCOPE)
    elseif(name MATCHES "^(FullScreenVS|Im3DVS)$")
        set(${out_profile} "vs_6_3" PARENT_SCOPE)
        set(${out_entry} "VSMain" PARENT_SCOPE)
    elseif(name MATCHES "^(GaussianFilterRGB3x3CS|GenerateMipsCS)$")
        set(${out_profile} "cs_6_3" PARENT_SCOPE)
        set(${out_entry} "mainCS" PARENT_SCOPE)
    elseif(name MATCHES "^(Im3DGSLines|Im3DGSPoints)$")
        set(${out_profile} "gs_6_3" PARENT_SCOPE)
        set(${out_entry} "GSMain" PARENT_SCOPE)
    elseif(name MATCHES "RayGen$" OR name MATCHES "RayTracing$" OR name STREQUAL "HitGroupReference")
        set(${out_profile} "lib_6_3" PARENT_SCOPE)
        set(${out_entry} "-" PARENT_SCOPE)
    else()
        set(${out_profile} "" PARENT_SCOPE)
        set(${out_entry} "" PARENT_SCOPE)
    endif()
endfunction()

# rt64_compile_shaders(<target> <shader_dir> <output_dir>)
function(rt64_compile_shaders target shader_dir output_dir)
    rt64_find_dxc()
    file(MAKE_DIRECTORY ${output_dir})

    file(GLOB shader_files ${shader_dir}/*.hlsl)
    # Every .hlsli is a potential dependency of every .hlsl, so a change to one
    # must rebuild all of them.
    file(GLOB shader_includes ${shader_dir}/*.hlsli)

    set(spv_outputs)
    foreach(src ${shader_files})
        get_filename_component(name ${src} NAME_WE)
        rt64_shader_profile(${name} profile entry)
        if(NOT profile)
            message(FATAL_ERROR
                "No shader profile mapped for ${name}.hlsl — add it to "
                "rt64_shader_profile in CompileShaders.cmake.")
        endif()

        set(out ${output_dir}/${name}.spv)
        set(entry_arg)
        if(NOT entry STREQUAL "-")
            set(entry_arg -E ${entry})
        endif()

        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E env
                    "LD_LIBRARY_PATH=${RT64_DXC_LIBRARY_DIR}:$ENV{LD_LIBRARY_PATH}"
                    ${RT64_DXC_EXECUTABLE}
                    -T ${profile} ${entry_arg}
                    -spirv -HV 2018 -fvk-use-dx-layout
                    -fspv-target-env=${RT64_SPV_TARGET_ENV}
                    -fvk-u-shift ${RT64_VK_U_SHIFT} 0
                    -fvk-t-shift ${RT64_VK_T_SHIFT} 0
                    -fvk-b-shift ${RT64_VK_B_SHIFT} 0
                    -fvk-s-shift ${RT64_VK_S_SHIFT} 0
                    -I ${shader_dir}
                    ${src} -Fo ${out}
            DEPENDS ${src} ${shader_includes}
            COMMENT "SPIR-V ${name}.spv (${profile})"
            VERBATIM)
        list(APPEND spv_outputs ${out})
    endforeach()

    add_custom_target(${target} ALL DEPENDS ${spv_outputs})
    set(RT64_SPV_OUTPUTS ${spv_outputs} PARENT_SCOPE)
endfunction()
