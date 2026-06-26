cmake_minimum_required(VERSION 3.16)
project(fast_cache_engine VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(FCE_ENABLE_ZSTD "Enable zstd codec when libzstd is available" ON)

if(FCE_ENABLE_ZSTD)
  find_path(ZSTD_INCLUDE_DIR zstd.h)
  find_library(ZSTD_LIBRARY NAMES zstd libzstd)
  if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    set(FCE_HAVE_ZSTD ON)
  endif()
endif()

set(FCE_PUBLIC_HEADERS
  include/fce_builder.h
  include/fce_cache.h
  include/fce_error.h
  include/fce_reader.h
  include/fce_schema.h
)

set(FCE_LIBRARY_SOURCES
  src/api/fce_builder_api.c
  src/api/fce_reader_api.c
  src/backends/fce_backend_direct.c
  src/backends/fce_backend_log.c
  src/backends/fce_backend_mph.c
  src/backends/fce_backend_radix.c
  src/backends/fce_backend_sorted.c
  src/codecs/fce_codecs.c
  src/core/fce_build_records.c
  src/core/fce_crc.c
  src/core/fce_file.c
  src/core/fce_hash.c
  src/core/fce_manifest.c
  src/core/fce_memory.c
  src/core/fce_planner.c
  src/core/fce_schema.c
  src/core/fce_version.c
)

add_library(fast_cache_engine
  ${FCE_LIBRARY_SOURCES}
  ${FCE_PUBLIC_HEADERS}
)

target_include_directories(fast_cache_engine
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)

if(WIN32)
  target_compile_definitions(fast_cache_engine PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(FCE_HAVE_ZSTD)
  target_compile_definitions(fast_cache_engine PRIVATE FCE_HAVE_ZSTD)
  target_include_directories(fast_cache_engine PRIVATE ${ZSTD_INCLUDE_DIR})
  target_link_libraries(fast_cache_engine PRIVATE ${ZSTD_LIBRARY})
endif()

add_library(fast_cache_engine_shared SHARED
  ${FCE_LIBRARY_SOURCES}
  ${FCE_PUBLIC_HEADERS}
)
target_include_directories(fast_cache_engine_shared
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)
target_compile_definitions(fast_cache_engine_shared PRIVATE FCE_BUILD_SHARED FCE_BUILDING_LIBRARY)
set_target_properties(fast_cache_engine_shared PROPERTIES
  OUTPUT_NAME fast_cache_engine
  ARCHIVE_OUTPUT_NAME fast_cache_engine_shared)
if(WIN32)
  target_compile_definitions(fast_cache_engine_shared PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(FCE_HAVE_ZSTD)
  target_compile_definitions(fast_cache_engine_shared PRIVATE FCE_HAVE_ZSTD)
  target_include_directories(fast_cache_engine_shared PRIVATE ${ZSTD_INCLUDE_DIR})
  target_link_libraries(fast_cache_engine_shared PRIVATE ${ZSTD_LIBRARY})
endif()

add_executable(fce src/cli/fce_cli.c)
target_link_libraries(fce PRIVATE fast_cache_engine)

install(TARGETS fast_cache_engine fast_cache_engine_shared fce
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
