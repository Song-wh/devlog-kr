# 유효 클럭 측정 (MASM 덧셈 체인). 단위 테스트에서도 링크한다
enable_language(ASM_MASM)
add_library(cpu_clock STATIC clock.cpp clockchain.asm)
target_include_directories(cpu_clock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cpu_clock PRIVATE bench_options)

add_executable(cpu-bench-runner main.cpp topology.cpp smbios.cpp envcheck.cpp ramspec.cpp throttle.cpp score.cpp pcie.cpp)
target_link_libraries(cpu-bench-runner PRIVATE bench_api bench_options cpu_clock int_kernels setupapi)
# 모듈은 LoadLibrary로 실행 중에 읽지만, 빌드 순서상 함께 만들어지게 한다
add_dependencies(cpu-bench-runner bench_int bench_mem)
target_compile_definitions(cpu-bench-runner PRIVATE CPU_BENCH_VERSION="${PROJECT_VERSION}")
