# frontend server libraries 
add_library(werewolf_server_pipe_lib STATIC ${CMAKE_CURRENT_SOURCE_DIR}/server.cpp)
target_link_libraries(werewolf_server_pipe_lib PRIVATE werewolf_core)

# frontend client libraries 
add_library(werewolf_client_pipe_lib STATIC ${CMAKE_CURRENT_SOURCE_DIR}/client.cpp)
target_link_libraries(werewolf_client_pipe_lib PRIVATE werewolf_core)

# pipe server and pipe clients
add_executable(werewolf_pipe_server ${CMAKE_CURRENT_SOURCE_DIR}/pipe_server.cpp)
target_link_libraries(werewolf_pipe_server PRIVATE werewolf_server_pipe_lib werewolf_pipe_backend)

add_executable(werewolf_pipe_client ${CMAKE_CURRENT_SOURCE_DIR}/pipe_client.cpp)
target_link_libraries(werewolf_pipe_client PRIVATE werewolf_client_pipe_lib werewolf_pipe_backend)

# mqueue server and mqueue clients
add_executable(werewolf_mqueue_server ${CMAKE_CURRENT_SOURCE_DIR}/mqueue_server.cpp)
target_link_libraries(werewolf_mqueue_server PRIVATE werewolf_server_pipe_lib werewolf_mqueue_backend)

add_executable(werewolf_mqueue_client ${CMAKE_CURRENT_SOURCE_DIR}/mqueue_client.cpp)
target_link_libraries(werewolf_mqueue_client PRIVATE werewolf_client_pipe_lib werewolf_mqueue_backend)