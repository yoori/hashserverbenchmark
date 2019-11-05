project(HashServerBenchmark)

add_executable(HashServerBenchmark
  main.cpp
  )

target_link_libraries(HashServerBenchmark
  boost_system
  boost_program_options
  ${CMAKE_THREAD_LIBS_INIT})

install(TARGETS HashServerBenchmark DESTINATION bin)
