add_library(xenia-debug-gdb STATIC)
xe_platform_sources(xenia-debug-gdb ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-debug-gdb PUBLIC imgui xenia-base xenia-cpu xenia-ui)
if(MSVC)
  # Prefer size over speed here; the GDB stub isn't performance-critical
  # and the codebase has historically built it with /Os /O1 to keep the
  # release binary footprint down.
  target_compile_options(xenia-debug-gdb PRIVATE
    $<$<CONFIG:Release>:/Os>
    $<$<CONFIG:Release>:/O1>
  )
endif()
xe_target_defaults(xenia-debug-gdb)
