CMAKE_MINIMUM_REQUIRED(VERSION 3.0)

PROJECT(libgvnc)

ADD_LIBRARY(gvnc 
	lib/ClientConnection.cpp
	lib/Server.cpp
	lib/Framebuffer.cpp
	lib/Encoder.cpp
        lib/net/EndPoint.cpp
        lib/net/SockAddr.cpp
        lib/net/Socket.cpp
)

standard_flags(gvnc)

TARGET_INCLUDE_DIRECTORIES(gvnc PUBLIC inc/)
