# Benchmarks are opt-in executables, not ctest targets, so the suite stays fast.
file(GLOB GBX_BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(GBX_BENCH_SOURCES)
  add_executable(glassbox_bench ${GBX_BENCH_SOURCES})
  target_link_libraries(glassbox_bench PRIVATE glassbox)
endif()
