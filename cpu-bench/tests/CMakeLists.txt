add_executable(int_kernels_test int_kernels_test.cpp)
target_link_libraries(int_kernels_test PRIVATE int_kernels bench_options)
add_test(NAME int_kernels COMMAND int_kernels_test)

# DLL을 실제로 읽어 짧게 돌려 보는 스모크 테스트 (checksum 불일치면 종료 코드 3으로 실패)
add_test(NAME runner_info COMMAND cpu-bench-runner info)
add_test(NAME runner_int_st COMMAND cpu-bench-runner run int --duration-ms 200 --runs 2)
add_test(NAME runner_int_mt COMMAND cpu-bench-runner run int --mt --duration-ms 200 --runs 2 --json)
