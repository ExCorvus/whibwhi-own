cmake_minimum_required(VERSION 3.16)
project(whiwhi_shell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Per-thread storage block the loader lends to a loaded image, living in the EXE's own thread-local section
# (index 0 - the same slot the console gives the game, so the game's hardcoded slot-0 reads and its
# index-variable reads hit the same block).
add_library(whiwhi_seat STATIC tls_seat.cpp)
target_compile_definitions(whiwhi_seat PUBLIC WHIWHI_THREAD_BLOCK_STATIC)
target_include_directories(whiwhi_seat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The console's memory library, answered on a PC. An image imports it by this file name.
add_library(xmem SHARED xmem.cpp)
target_compile_definitions(xmem PRIVATE WHIWHI_XMEM_BUILD)
target_link_libraries(xmem PRIVATE onecore)

# The console's graphics library: the three imported entry points answered on a PC (a wrapped real D3D12
# device behind D3D12XboxCreateDevice).
add_library(d3d12_xs SHARED d3d12_xs.cpp inflate.cpp shader_container.cpp shader_isa.cpp shader_names.cpp
                            shader_program.cpp shader_shape.cpp shader_walk.cpp shader_flow.cpp
                            shader_emit_flow.cpp shader_access.cpp shader_dxbc.cpp shader_pc.cpp)
target_compile_definitions(d3d12_xs PRIVATE WHIWHI_D3D12_XS_BUILD)
target_link_libraries(d3d12_xs PRIVATE onecore)

# The console's texture-computer library and ray-tracing builder library, as honest skeletons.
add_library(xg_xs SHARED xg_xs.cpp)
target_compile_definitions(xg_xs PRIVATE WHIWHI_XG_XS_BUILD)
target_link_libraries(xg_xs PRIVATE onecore)

add_library(XboxBvhBuilder SHARED xbox_bvh_builder.cpp)
target_compile_definitions(XboxBvhBuilder PRIVATE WHIWHI_XBOX_BVH_BUILDER_BUILD)
target_link_libraries(XboxBvhBuilder PRIVATE onecore)

# The console's audio libraries, provided as skeletons: every imported name exists, every one of them answers
# "not provided" (a negative HRESULT, a 0 count, a null address) and says so once on stderr. No device is faked.
add_library(AcpHal SHARED acp_hal.cpp)
target_compile_definitions(AcpHal PRIVATE WHIWHI_ACP_HAL_BUILD)
target_link_libraries(AcpHal PRIVATE onecore)

add_library(xapu SHARED xapu.cpp)
target_compile_definitions(xapu PRIVATE WHIWHI_XAPU_BUILD)
target_link_libraries(xapu PRIVATE onecore)

# The console's audio DSP library: five entry points. A skeleton: each one logs its name and arguments once and
# answers failure, until the shell has a PC path for the APU's command channel.
add_library(chad SHARED chad.cpp)
target_compile_definitions(chad PRIVATE WHIWHI_CHAD_BUILD)

# The console's storage library: the one imported entry point (DStorageGetFactory) answered on a PC, forwarded
# to the PC's dstorage.dll when that library is installed.
add_library(xcrdapi SHARED xcrdapi.cpp inflate.cpp)
target_compile_definitions(xcrdapi PRIVATE WHIWHI_XCRDAPI_BUILD)
target_link_libraries(xcrdapi PRIVATE onecore)

# The Xbox Game Runtime: the game LoadLibrary's it and resolves Initialize/Query/Uninitialize ApiImpl.
add_library(xgameruntime SHARED xgameruntime.cpp)
target_compile_definitions(xgameruntime PRIVATE WHIWHI_XGAMERUNTIME_BUILD)
target_link_libraries(xgameruntime PRIVATE onecore)

# Shell core: image mapper, import table reader, image loader, host-library resolver, console imports, flat
# hosting model.
add_library(whiwhi_shell STATIC console.cpp hosting.cpp host_libraries.cpp image.cpp imports.cpp inflate.cpp
                               loader.cpp offline.cpp shader_access.cpp shader_container.cpp shader_dxbc.cpp
                               shader_emit_flow.cpp shader_flow.cpp shader_isa.cpp shader_meaning.cpp
                               shader_names.cpp shader_pc.cpp shader_program.cpp shader_shape.cpp
                               shader_translate.cpp shader_walk.cpp spirv.cpp)
target_include_directories(whiwhi_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(whiwhi_shell PUBLIC whiwhi_seat)

add_executable(list_imports tools/list_imports.cpp)
target_link_libraries(list_imports PRIVATE whiwhi_shell)

add_executable(describe_image tools/describe_image.cpp)
target_link_libraries(describe_image PRIVATE whiwhi_shell)

add_executable(connect_imports tools/connect_imports.cpp)
target_link_libraries(connect_imports PRIVATE whiwhi_shell)
add_dependencies(connect_imports xmem d3d12_xs AcpHal xapu chad xcrdapi xg_xs XboxBvhBuilder xgameruntime)

add_executable(boot_image tools/boot_image.cpp)
target_link_libraries(boot_image PRIVATE whiwhi_shell)
add_dependencies(boot_image xmem d3d12_xs AcpHal xapu chad xcrdapi xg_xs XboxBvhBuilder xgameruntime)

# Proves the GPU reads a plain host buffer zero-copy (VK_EXT_external_memory_host). Needs the Vulkan SDK.
if(EXISTS "C:/VulkanSDK")
    file(GLOB _vk_inc "C:/VulkanSDK/*/Include")
    list(GET _vk_inc 0 _vk_inc0)
    file(GLOB _vk_lib "C:/VulkanSDK/*/Lib")
    list(GET _vk_lib 0 _vk_lib0)
    add_executable(vkhost_probe tools/vkhost_probe.cpp)
    target_include_directories(vkhost_probe PRIVATE "${_vk_inc0}")
    target_link_directories(vkhost_probe PRIVATE "${_vk_lib0}")
    target_link_libraries(vkhost_probe PRIVATE vulkan-1)
endif()

enable_testing()

add_executable(test_hosting tests/test_hosting.cpp)
target_link_libraries(test_hosting PRIVATE whiwhi_shell)
add_test(NAME hosting COMMAND test_hosting)

add_executable(test_image tests/test_image.cpp)
target_link_libraries(test_image PRIVATE whiwhi_shell)
add_test(NAME image COMMAND test_image)

add_executable(test_imports tests/test_imports.cpp)
target_link_libraries(test_imports PRIVATE whiwhi_shell)
add_test(NAME imports COMMAND test_imports)

add_executable(test_loader tests/test_loader.cpp)
target_link_libraries(test_loader PRIVATE whiwhi_shell)
add_test(NAME loader COMMAND test_loader)

add_executable(test_thread_storage tests/test_thread_storage.cpp)
target_link_libraries(test_thread_storage PRIVATE whiwhi_shell)
add_test(NAME thread_storage COMMAND test_thread_storage)

add_executable(test_console tests/test_console.cpp)
target_link_libraries(test_console PRIVATE whiwhi_shell)
add_dependencies(test_console xmem)
add_test(NAME console COMMAND test_console)

add_executable(test_xmem tests/test_xmem.cpp)
target_include_directories(test_xmem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_xmem PRIVATE xmem)
add_test(NAME xmem COMMAND test_xmem)

add_executable(test_d3d12_xs tests/test_d3d12_xs.cpp)
add_dependencies(test_d3d12_xs d3d12_xs)
add_test(NAME d3d12_xs COMMAND test_d3d12_xs)

add_executable(test_shader tests/test_shader.cpp)
target_link_libraries(test_shader PRIVATE whiwhi_shell)
add_test(NAME shader COMMAND test_shader)

add_executable(test_shader_container tests/test_shader_container.cpp)
target_link_libraries(test_shader_container PRIVATE whiwhi_shell)
add_test(NAME shader_container COMMAND test_shader_container)

# The writer for a DXBC container holding a shader-model-5.1 program; the system disassembler checks it.
add_executable(test_shader_dxbc tests/test_shader_dxbc.cpp)
target_link_libraries(test_shader_dxbc PRIVATE whiwhi_shell)
add_test(NAME shader_dxbc COMMAND test_shader_dxbc)

add_executable(test_shader_walk tests/test_shader_walk.cpp)
target_link_libraries(test_shader_walk PRIVATE whiwhi_shell)
add_test(NAME shader_walk COMMAND test_shader_walk)

# The public names of the instructions the walk reads, and the groups a translator works through.
add_executable(test_shader_names tests/test_shader_names.cpp)
target_link_libraries(test_shader_names PRIVATE whiwhi_shell)
add_test(NAME shader_names COMMAND test_shader_names)

add_executable(test_shader_shape tests/test_shader_shape.cpp)
target_link_libraries(test_shader_shape PRIVATE whiwhi_shell)
add_test(NAME shader_shape COMMAND test_shader_shape)

# The packed program inside a shader container, found and unpacked in memory (hand-built containers).
add_executable(test_shader_program tests/test_shader_program.cpp)
target_link_libraries(test_shader_program PRIVATE whiwhi_shell)
add_test(NAME shader_program COMMAND test_shader_program)

# The control flow of a decoded program: blocks, edges, dominators, loops, and the region tree (hand-built words).
add_executable(test_shader_flow tests/test_shader_flow.cpp)
target_link_libraries(test_shader_flow PRIVATE whiwhi_shell)
add_test(NAME shader_flow COMMAND test_shader_flow)

# Any control-flow graph as code a shader language can run: the same block order as the graph under the same
# branch decisions, for hand-built shapes and 2,000 random graphs (no GPU, no game data).
add_executable(test_shader_emit_flow tests/test_shader_emit_flow.cpp)
target_link_libraries(test_shader_emit_flow PRIVATE whiwhi_shell)
add_test(NAME shader_emit_flow COMMAND test_shader_emit_flow)

# The pass that writes a PC shader for a console program: the written program against the reference interpreter
# on random hand-built programs, single instructions, a pixel and a vertex program read back by the system's
# shader disassembler (no GPU, no game data).
add_executable(test_shader_pc tests/test_shader_pc.cpp)
target_link_libraries(test_shader_pc PRIVATE whiwhi_shell)
add_test(NAME shader_pc COMMAND test_shader_pc)

# What the memory, image, interpolation, export and whole-wave instructions touch, on hand-built words.
add_executable(test_shader_access tests/test_shader_access.cpp)
target_link_libraries(test_shader_access PRIVATE whiwhi_shell)
add_test(NAME shader_access COMMAND test_shader_access)

# What the covered arithmetic, move, compare and bit instructions compute, on one hand-built lane.
add_executable(test_shader_meaning tests/test_shader_meaning.cpp)
target_link_libraries(test_shader_meaning PRIVATE whiwhi_shell)
add_test(NAME shader_meaning COMMAND test_shader_meaning)

# The network boundary: every imported network name is either refused with its documented failure value and error,
# or passed through; the log line appears once per function; the counters count every call.
add_executable(test_offline tests/test_offline.cpp)
target_link_libraries(test_offline PRIVATE whiwhi_shell ws2_32)
add_test(NAME offline COMMAND test_offline)

# The audio libraries: the built DLLs beside this test must carry every imported name and answer not-provided.
add_executable(test_acpxapu tests/test_acpxapu.cpp)
add_dependencies(test_acpxapu AcpHal xapu)
add_test(NAME acpxapu COMMAND test_acpxapu)

add_executable(test_chad tests/test_chad.cpp)
target_include_directories(test_chad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(test_chad chad)
add_test(NAME chad COMMAND test_chad $<TARGET_FILE:chad>)

add_executable(test_xcrdapi tests/test_xcrdapi.cpp)
add_dependencies(test_xcrdapi xcrdapi)
add_test(NAME xcrdapi COMMAND test_xcrdapi)

# The zlib decoder the storage library uses, tested on its own.
add_executable(test_inflate tests/test_inflate.cpp inflate.cpp)
target_include_directories(test_inflate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME inflate COMMAND test_inflate)

add_executable(test_gfx_stubs tests/test_gfx_stubs.cpp)
add_dependencies(test_gfx_stubs xg_xs XboxBvhBuilder xmem xgameruntime)
add_test(NAME gfx_stubs COMMAND test_gfx_stubs)

# The texture layout library: layout numbers worked out by hand, and copies in and out of made-up memory.
add_executable(test_xg tests/test_xg.cpp)
add_dependencies(test_xg xg_xs)
add_test(NAME xg COMMAND test_xg)
