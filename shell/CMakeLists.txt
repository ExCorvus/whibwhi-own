cmake_minimum_required(VERSION 3.16)
project(whiwhi_shell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Per-thread storage block the loader lends to a loaded image. Built without the C runtime so that its
# thread-local section holds only that block.
add_library(whiwhi_tls SHARED thread_block.cpp)
target_compile_definitions(whiwhi_tls PRIVATE WHIWHI_THREAD_BLOCK_BUILD)
target_compile_options(whiwhi_tls PRIVATE /GS-)
target_link_options(whiwhi_tls PRIVATE /NODEFAULTLIB /ENTRY:whiwhi_thread_block_entry)
target_link_libraries(whiwhi_tls PRIVATE kernel32)

# The console's memory library, answered on a PC. An image imports it by this file name.
add_library(xmem SHARED xmem.cpp)
target_compile_definitions(xmem PRIVATE WHIWHI_XMEM_BUILD)
target_link_libraries(xmem PRIVATE onecore)

# The console's graphics library: the three imported entry points answered on a PC (a wrapped real D3D12
# device behind D3D12XboxCreateDevice).
add_library(d3d12_xs SHARED d3d12_xs.cpp)
target_compile_definitions(d3d12_xs PRIVATE WHIWHI_D3D12_XS_BUILD)
target_link_libraries(d3d12_xs PRIVATE onecore)

# The console's audio libraries, provided as skeletons: every imported name exists, every one of them answers
# "not provided" (a negative HRESULT, a 0 count, a null address) and says so once on stderr. No device is faked.
add_library(AcpHal SHARED acp_hal.cpp)
target_compile_definitions(AcpHal PRIVATE WHIWHI_ACP_HAL_BUILD)
target_link_libraries(AcpHal PRIVATE onecore)

add_library(xapu SHARED xapu.cpp)
target_compile_definitions(xapu PRIVATE WHIWHI_XAPU_BUILD)
target_link_libraries(xapu PRIVATE onecore)

# Shell core: image mapper, import table reader, image loader, host-library resolver, console imports, flat
# hosting model.
add_library(whiwhi_shell STATIC console.cpp hosting.cpp host_libraries.cpp image.cpp imports.cpp loader.cpp
                               shader_isa.cpp shader_translate.cpp spirv.cpp)
target_include_directories(whiwhi_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(whiwhi_shell PUBLIC whiwhi_tls)

add_executable(list_imports tools/list_imports.cpp)
target_link_libraries(list_imports PRIVATE whiwhi_shell)

add_executable(describe_image tools/describe_image.cpp)
target_link_libraries(describe_image PRIVATE whiwhi_shell)

add_executable(connect_imports tools/connect_imports.cpp)
target_link_libraries(connect_imports PRIVATE whiwhi_shell)
add_dependencies(connect_imports xmem d3d12_xs AcpHal xapu)

add_executable(boot_image tools/boot_image.cpp)
target_link_libraries(boot_image PRIVATE whiwhi_shell)
add_dependencies(boot_image xmem d3d12_xs AcpHal xapu)

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

# The audio libraries: the built DLLs beside this test must carry every imported name and answer not-provided.
add_executable(test_acpxapu tests/test_acpxapu.cpp)
add_dependencies(test_acpxapu AcpHal xapu)
add_test(NAME acpxapu COMMAND test_acpxapu)
