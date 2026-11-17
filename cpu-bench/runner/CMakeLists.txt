add_executable(cpu-bench-runner main.cpp topology.cpp)
target_link_libraries(cpu-bench-runner PRIVATE bench_api bench_options)
# 모듈은 LoadLibrary로 실행 중에 읽지만, 빌드 순서상 함께 만들어지게 한다
add_dependencies(cpu-bench-runner bench_int)
