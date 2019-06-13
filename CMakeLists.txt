# CMakeLists.txt for Http
#
# © 2018 by Richard Walters

cmake_minimum_required(VERSION 3.8)
set(This Http)

set(Headers
    include/Http/ChunkedBody.hpp
    include/Http/Client.hpp
    include/Http/ClientTransport.hpp
    include/Http/Connection.hpp
    include/Http/IClient.hpp
    include/Http/IServer.hpp
    include/Http/Request.hpp
    include/Http/Response.hpp
    include/Http/Server.hpp
    include/Http/ServerTransport.hpp
    include/Http/TimeKeeper.hpp
)

set(Sources
    src/ChunkedBody.cpp
    src/Client.cpp
    src/Deflate.cpp
    src/Deflate.hpp
    src/Inflate.cpp
    src/Inflate.hpp
    src/Request.cpp
    src/Response.cpp
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
    Timekeeping
    Uri
    zlibstatic
)

add_subdirectory(test)
