cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 17)

project(jacknetumpd VERSION 0.2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
include(FindPkgConfig)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/modules/NetUMP)

add_executable(${PROJECT_NAME})

# Add sources
target_sources(${PROJECT_NAME}
    PRIVATE
    jacknetumpd.cpp
    Endpoint.cpp
    UMP_mDNS.cpp
)

# Link libraries 
target_link_libraries(${PROJECT_NAME}
    PRIVATE
    NetUMP
    PkgConfig::JACK
)

if(UNIX AND NOT APPLE)
	target_link_libraries(${PROJECT_NAME} PRIVATE dns_sd)
endif()

if(WIN32)
    target_compile_definitions(${PROJECT_NAME}
        PUBLIC
            __TARGET_WIN__=1
    )
elseif(APPLE)
    target_compile_definitions(${PROJECT_NAME}
        PUBLIC
            __TARGET_MAC__=1
    )
elseif(UNIX)
    target_compile_definitions(${PROJECT_NAME}
        PUBLIC
            __TARGET_LINUX__=1
    )
else()
    error("Unsupported platform")
endif()
