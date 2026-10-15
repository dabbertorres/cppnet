# Each target is a libFuzzer executable. Run one for longer with a corpus directory (which it adds to as it goes), e.g.:
#
#   mkdir -p fuzz-corpus/url && ./build/fuzz/Debug/fuzz_url -max_total_time=300 -max_len=256 fuzz-corpus/url
#
# Each is also a short ctest run, labelled "fuzz" (exclude them with `ctest -LE fuzz`).
#
# The sources under test are compiled into each target, so that libFuzzer can trace their coverage.
function(add_fuzzer name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/include/")
  target_compile_features(${name} PRIVATE cxx_std_23)
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)

  add_test(NAME ${name} COMMAND ${name} -runs=100000 -max_len=256)
  set_tests_properties(${name} PROPERTIES LABELS fuzz)
endfunction()

add_fuzzer(fuzz_url url.cpp "${PROJECT_SOURCE_DIR}/src/url.cpp")
add_fuzzer(
  fuzz_request_target
  request_target.cpp
  "${PROJECT_SOURCE_DIR}/src/url.cpp"
  "${PROJECT_SOURCE_DIR}/src/http/request_target.cpp"
)
