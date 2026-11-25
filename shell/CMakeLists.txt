cmake_minimum_required(VERSION 3.16)
project(whiwhi_shell LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shell core: image mapper, import table reader, flat hosting model.
add_library(whiwhi_shell STATIC hosting.cpp image.cpp imports.cpp)
target_include_directories(whiwhi_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(list_imports tools/list_imports.cpp)
target_link_libraries(list_imports PRIVATE whiwhi_shell)

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
