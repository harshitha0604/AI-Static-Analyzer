cmake_minimum_required(VERSION 3.13)
project(MyClangTool)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 🔥 FORCE MSVC TO USE STATIC RUNTIME (/MT)
if (MSVC)
    foreach(flag_var
        CMAKE_C_FLAGS_RELEASE
        CMAKE_C_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_DEBUG)
        string(REPLACE "/MDd" "/MTd" ${flag_var} "${${flag_var}}")
        string(REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
        string(REPLACE "/MTd" "/MT" ${flag_var} "${${flag_var}}")
    endforeach()

    add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)
endif()

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_executable(my-checker
    main.cpp
    MyCheck.cpp
)

target_link_libraries(my-checker
    clangTooling
    clangBasic
    clangASTMatchers
)
