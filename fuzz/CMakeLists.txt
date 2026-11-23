# Fuzz harnesses are plain executables run by ctest; see fuzz/README.md.
file(GLOB GBX_FUZZ_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(GBX_FUZZ_SOURCES)
  add_executable(glassbox_fuzz ${GBX_FUZZ_SOURCES})
  target_link_libraries(glassbox_fuzz PRIVATE glassbox)
  add_test(NAME fuzz_smoke COMMAND glassbox_fuzz)
  set_tests_properties(fuzz_smoke PROPERTIES TIMEOUT 300)
endif()
