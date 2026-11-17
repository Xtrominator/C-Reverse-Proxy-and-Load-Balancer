# ---------------------------------------------------------------------------
# CMakeLists.txt  --  tells CMake how to build the project.
#
# CMake does NOT compile code itself. It reads this file and generates a
# build system (usually Makefiles) that then runs the compiler for us.
#
# Build it with:
#     cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#     cmake --build build -j
# The finished program is  build/proxy
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.10)
project(epoll_proxy CXX)

# We use C++17 features (std::optional-free, but structured code, inline vars...).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# If you did not choose a build type, default to Release (-O2, fast).
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# "Threads" gives us the right flags for POSIX threads (std::thread uses them).
find_package(Threads REQUIRED)

add_executable(proxy
  src/main.cpp
  src/util.cpp
  src/md5.cpp
  src/circuit_breaker.cpp
  src/balancer.cpp
  src/health_checker.cpp
  src/worker.cpp
)

# Turn on compiler warnings. Warnings catch many beginner mistakes.
target_compile_options(proxy PRIVATE -Wall -Wextra)
target_link_libraries(proxy PRIVATE Threads::Threads)
