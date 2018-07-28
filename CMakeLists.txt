# CMakeLists.txt for Http
#
# © 2018 by Richard Walters

cmake_minimum_required(VERSION 3.8)
set(This Http)

set(Headers
    include/Http/Client.hpp
    include/Http/Connection.hpp
    include/Http/IServer.hpp
    include/Http/Request.hpp
    include/Http/Server.hpp
    include/Http/ServerTransport.hpp
)

set(Sources
    src/Client.cpp
    src/Request.cpp
    src/Server.cpp
)

add_library(${This} STATIC ${Sources} ${Headers})
set_target_properties(${This} PROPERTIES
    FOLDER Libraries
)

target_include_directories(${This} PUBLIC include)

target_link_libraries(${This} PUBLIC
    MessageHeaders
    SystemAbstractions
    Uri
)

add_subdirectory(test)
