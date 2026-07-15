# CMakeLists.txt — production-standard build (use once you `pacman -S mingw-w64-x86_64-cmake`).
#
#   cmake -S . -B build -G "MinGW Makefiles"
#   cmake --build build
#   ./build/payment
#
cmake_minimum_required(VERSION 3.16)
project(PaymentSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# All headers live under include/. Sources under src/.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(payment ${SOURCES})
target_include_directories(payment PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Threads (std::thread / std::mutex).
find_package(Threads REQUIRED)
target_link_libraries(payment PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# When you wire up real infrastructure later, you'll add (commented for now):
#   find_package(libpqxx)   # PostgreSQL
#   find_package(hiredis)   # Redis
#   find_package(RdKafka)   # Kafka
#   find_package(rabbitmq-c)# RabbitMQ
# and link them ONLY to the adapter targets, never to the core domain.
# ---------------------------------------------------------------------------
