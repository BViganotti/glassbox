# Fuzz harnesses are plain executables run by ctest with fixed seeds, so every
# run is reproducible. See fuzz/fuzz_main.cpp for the modes.
file(GLOB GBX_FUZZ_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(GBX_FUZZ_SOURCES)
  add_executable(glassbox_fuzz ${GBX_FUZZ_SOURCES})
  target_link_libraries(glassbox_fuzz PRIVATE glassbox)
  foreach(mode reader query json http roundtrip)
    add_test(NAME fuzz_${mode} COMMAND glassbox_fuzz ${mode} 160)
    set_tests_properties(fuzz_${mode} PROPERTIES TIMEOUT 180)
  endforeach()
endif()
