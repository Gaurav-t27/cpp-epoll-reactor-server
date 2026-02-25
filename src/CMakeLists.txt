# ============================================================================
# Helper function to create a library with standard settings
# ============================================================================
function(add_reactor_library TARGET_NAME)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES DEPENDS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    add_library(${TARGET_NAME} STATIC ${ARG_SOURCES})
    
    target_include_directories(${TARGET_NAME}
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include/${PROJECT_NAME}>
    )
    
    if(ARG_DEPENDS)
        target_link_libraries(${TARGET_NAME} PUBLIC ${ARG_DEPENDS})
    endif()
endfunction()

# ============================================================================
# Core Libraries
# ============================================================================
# Individual component libraries for modularity
add_reactor_library(socket_lib SOURCES socket.cpp)

add_reactor_library(reactor_lib SOURCES reactor.cpp)

add_reactor_library(tcp_server_lib 
    SOURCES tcp_server.cpp
    DEPENDS socket_lib reactor_lib Threads::Threads
)

# ============================================================================
# Executables
# ============================================================================
add_executable(tcp_server main.cpp)
target_include_directories(tcp_server 
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(tcp_server PRIVATE tcp_server_lib)

# ============================================================================
# Installation
# ============================================================================
install(TARGETS tcp_server socket_lib reactor_lib tcp_server_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
