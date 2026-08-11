cmake_minimum_required(VERSION 3.16)
project(cpp_postgres_lru_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- ThreadSanitizer ----
# Build with: cmake -B build-tsan -DENABLE_TSAN=ON && cmake --build build-tsan
# Then run:   ./build-tsan/run_tests
option(ENABLE_TSAN "Build with ThreadSanitizer instrumentation" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)
find_package(Threads REQUIRED)

include_directories(include ${PQXX_INCLUDE_DIRS})

# ---- Main application executable (requires PostgreSQL + libpqxx) ----
add_executable(lru_db_engine
    src/main.cpp
    src/LRUCache.cpp
    src/DatabaseManager.cpp
)
target_link_libraries(lru_db_engine ${PQXX_LIBRARIES} Threads::Threads)

# ---- Test executable (pure LRUCache logic, no DB required) ----
add_executable(run_tests
    tests/test_engine.cpp
    src/LRUCache.cpp
)
target_link_libraries(run_tests Threads::Threads)

# ---- Benchmark executable (requires PostgreSQL + libpqxx) ----
add_executable(benchmark
    benchmarks/benchmark.cpp
    src/LRUCache.cpp
    src/DatabaseManager.cpp
)
target_link_libraries(benchmark ${PQXX_LIBRARIES} Threads::Threads)

enable_testing()
add_test(NAME LRUCacheTests COMMAND run_tests)
