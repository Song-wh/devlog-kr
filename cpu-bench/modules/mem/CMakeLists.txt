# 계산 커널 (단위 테스트에서도 링크)
add_library(mem_kernels STATIC mem_kernels.cpp)
target_include_directories(mem_kernels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mem_kernels PRIVATE bench_options)

# 부하 모듈 DLL: bench_mem.dll
add_library(bench_mem SHARED mem_module.cpp)
set_target_properties(bench_mem PROPERTIES PREFIX "")
target_link_libraries(bench_mem PRIVATE bench_harness bench_options mem_kernels)
