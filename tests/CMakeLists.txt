add_executable(wg_tests main.cpp)
target_link_libraries(wg_tests PRIVATE wg)
add_test(NAME wg_tests COMMAND wg_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
