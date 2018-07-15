# CMakeLists.txt for Http
#
# © 2018 by Richard Walters

cmake_minimum_required(VERSION 3.8)
set(This Http)

set(Headers
    include/Http/Server.hpp
    include/Http/Client.hpp
)

set(Sources
    src/Server.cpp
    src/Client.cpp
)

add_library(${This} STATIC ${Sources} ${Headers})
set_target_properties(${This} PROPERTIES
    FOLDER Libraries
)

target_include_directories(${This} PUBLIC include)

target_link_libraries(${This} PUBLIC
    MessageHeaders
    Uri
)

add_subdirectory(test)
