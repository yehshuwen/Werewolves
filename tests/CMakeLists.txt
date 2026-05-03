# for gtest
include(FetchContent)

# download googletest
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)

# build gtest and add to project
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(test_template ${CMAKE_CURRENT_SOURCE_DIR}/template/test_template.cpp)
add_executable(test_pipe ${CMAKE_CURRENT_SOURCE_DIR}/pipe/test_pipe.cpp)
add_executable(test_unit ${CMAKE_CURRENT_SOURCE_DIR}/game/test_unit.cpp)
add_executable(test_game ${CMAKE_CURRENT_SOURCE_DIR}/game/test_game.cpp)

target_link_libraries(test_template PUBLIC werewolf_template_backend GTest::gtest_main)
target_link_libraries(test_pipe PUBLIC werewolf_pipe_backend GTest::gtest_main)
target_link_libraries(test_unit PUBLIC werewolf_core GTest::gtest_main GTest::gmock_main)
target_link_libraries(test_game PUBLIC werewolf_core GTest::gtest_main GTest::gmock_main)

target_include_directories(test_template PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/utils)
target_include_directories(test_pipe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/utils)
target_include_directories(test_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/utils ${CMAKE_CURRENT_SOURCE_DIR}/game)
target_include_directories(test_game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/utils ${CMAKE_CURRENT_SOURCE_DIR}/game)

include(GoogleTest)

gtest_discover_tests(test_template)
gtest_discover_tests(test_pipe)
gtest_discover_tests(test_unit)
gtest_discover_tests(test_game)

# add e2e tests
add_test(
    NAME test_pipe_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/pipe/e2e/test_pipe_e2e.sh
)
add_test(
    NAME test_mqueue_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/mqueue/e2e/test_mqueue_e2e.sh
)

# mqueue
add_executable(test_mqueue ${CMAKE_CURRENT_SOURCE_DIR}/mqueue/test_mqueue.cpp)
target_link_libraries(test_mqueue PUBLIC werewolf_mqueue_backend GTest::gtest_main)
gtest_discover_tests(test_mqueue)