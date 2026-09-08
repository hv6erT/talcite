file(GLOB TALCITE_DEMOS CONFIGURE_DEPENDS "*.cpp")

foreach(one IN LISTS TALCITE_DEMOS)
  get_filename_component(named "${one}" NAME_WE)

  add_executable(talcite-${named} "${one}")
  target_link_libraries(talcite-${named} PRIVATE talcite)
endforeach()
