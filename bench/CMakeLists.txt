# Benchmarks, for comparing implementation choices (see each one's comments). They're only
# meaningful in an optimized build, e.g.:
#
# cmake --build build --config Release --target bench_task && ./build/bench/Release/bench_task
#
# They aren't tests: nothing runs them automatically.
function(add_benchmark name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include/")
  target_compile_features(${name} PRIVATE cxx_std_23)
endfunction()

add_benchmark(bench_task coro/task.cpp)

add_benchmark(bench_notifier coro/notifier.cpp)
target_link_libraries(bench_notifier PRIVATE net)

add_benchmark(bench_router http/router.cpp)
target_link_libraries(bench_router PRIVATE net)

add_benchmark(bench_request http/request.cpp)
target_link_libraries(bench_request PRIVATE net)

add_benchmark(bench_client http/client.cpp)
target_link_libraries(bench_client PRIVATE net)

add_benchmark(bench_websocket websocket/echo.cpp)
target_link_libraries(bench_websocket PRIVATE net)

# with TLS: full and resumed handshakes (it makes its certificates with Botan).
if(NET_TLS)
  add_benchmark(bench_tls tls/handshake.cpp)
  target_link_libraries(bench_tls PRIVATE net ${NET_BOTAN_TARGET})
endif()

# TechEmpower's plaintext and JSON tests: bench/web/run.sh runs it, and other servers, under wrk.
add_benchmark(bench_web web/cppnet/main.cpp)
target_link_libraries(bench_web PRIVATE net glaze::glaze)
