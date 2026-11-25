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

# Shell core: image mapper, import table reader, image loader, host-library resolver, flat hosting model.
add_library(whiwhi_shell STATIC hosting.cpp host_libraries.cpp image.cpp imports.cpp loader.cpp)
target_include_directories(whiwhi_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(whiwhi_shell PUBLIC whiwhi_tls)

add_executable(list_imports tools/list_imports.cpp)
target_link_libraries(list_imports PRIVATE whiwhi_shell)

add_executable(describe_image tools/describe_image.cpp)
target_link_libraries(describe_image PRIVATE whiwhi_shell)

add_executable(connect_imports tools/connect_imports.cpp)
target_link_libraries(connect_imports PRIVATE whiwhi_shell)

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
