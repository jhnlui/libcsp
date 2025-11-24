cmake_minimum_required(VERSION 3.10)

find_package(Threads REQUIRED)

add_executable(cpp_csp_server_test
    src/CSPServer.cpp
    src/main.cpp
)

target_include_directories(cpp_csp_server_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(cpp_csp_server_test PRIVATE csp Threads::Threads)

target_compile_features(cpp_csp_server_test PRIVATE cxx_std_14)
