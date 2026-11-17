# 계산 커널 (단위 테스트에서도 링크)
add_library(int_kernels STATIC int_kernels.cpp)
target_include_directories(int_kernels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(int_kernels PRIVATE bench_options)

# 부하 모듈 DLL: bench_int.dll
add_library(bench_int SHARED int_module.cpp)
set_target_properties(bench_int PROPERTIES PREFIX "")
target_link_libraries(bench_int PRIVATE bench_api bench_options int_kernels)
