# Benchmarks, for comparing implementation choices (see each one's comments). They're only meaningful in an optimized
# build, e.g.:
#
#   cmake --build build --config Release --target bench_task && ./build/bench/Release/bench_task
#
# They aren't tests: nothing runs them automatically.
function(add_benchmark name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include/")
  target_compile_features(${name} PRIVATE cxx_std_23)
endfunction()

add_benchmark(bench_task coro/task.cpp)
