cmake_minimum_required(VERSION 3.20)
project(polaris LANGUAGES CXX)

find_package(Threads REQUIRED)
add_executable(shopping-sessions
    src/main.cpp
    src/common.cpp
    src/csv.cpp
    src/ingest.cpp
    src/identity.cpp
    src/events.cpp
    src/attribution.cpp
    src/parquet.cpp
)
target_link_libraries(shopping-sessions PRIVATE Threads::Threads)

set(polaris_targets shopping-sessions)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    target_compile_options(shopping-sessions PRIVATE -march=x86-64-v2 -mtune=cascadelake)
    # Compile the whole scanning loop for each ISA, then dispatch by CPU feature
    # once per range. The rest of the executable stays on the baseline ISA.
    foreach(isa IN ITEMS avx2 avx512)
        add_library(ingest_${isa} OBJECT src/ingest.cpp)
        string(TOUPPER "${isa}" isa_upper)
        target_compile_definitions(ingest_${isa} PRIVATE POLARIS_${isa_upper})
        target_compile_options(ingest_${isa} PRIVATE -march=x86-64-v2 -mtune=cascadelake)
        target_link_libraries(shopping-sessions PRIVATE ingest_${isa})
        list(APPEND polaris_targets ingest_${isa})
    endforeach()
    target_compile_options(ingest_avx2 PRIVATE -mavx2)
    target_compile_options(ingest_avx512 PRIVATE -mavx512bw)
endif()

foreach(target IN LISTS polaris_targets)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

option(POLARIS_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
if(POLARIS_SANITIZE)
    foreach(target IN LISTS polaris_targets)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()
    target_link_options(shopping-sessions PRIVATE -fsanitize=address,undefined)
else()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported)
    if(ipo_supported)
        set_property(TARGET ${polaris_targets} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endif()
