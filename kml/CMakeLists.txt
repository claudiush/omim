project(kml)

set(
  SRC
  header_binary.hpp
  serdes.cpp
  serdes.hpp
  serdes_binary.cpp
  serdes_binary.hpp
  type_utils.cpp
  type_utils.hpp
  types.hpp
  types_v3.hpp
  types_v6.hpp
  visitors.hpp
)

omim_add_library(${PROJECT_NAME} ${SRC})

omim_add_pybindings_subdirectory(pykmlib)
omim_add_test_subdirectory(kml_tests)
