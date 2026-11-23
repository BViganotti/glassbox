# Tests are built from the glob so that each vertical slice only needs to add a
# file. One binary keeps link times low; ctest reports every failure with logs.
file(GLOB GBX_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(FILTER GBX_TEST_SOURCES EXCLUDE REGEX "test_main\\.cpp$")

add_executable(glassbox_tests test_main.cpp ${GBX_TEST_SOURCES})
target_link_libraries(glassbox_tests PRIVATE glassbox)
add_test(NAME unit COMMAND glassbox_tests)
set_tests_properties(unit PROPERTIES TIMEOUT 300)
