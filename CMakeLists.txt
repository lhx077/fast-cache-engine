cmake_minimum_required(VERSION 3.16)
project(fast_cache_engine VERSION 1.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(FCE_ENABLE_ZSTD "Enable zstd codec when libzstd is available" ON)
option(FCE_BUILD_STATIC "Build the static Fast Cache Engine library" ON)
option(FCE_BUILD_SHARED "Build the shared Fast Cache Engine library" ON)
option(FCE_BUILD_CLI "Build the fce command line tool" ON)

if(NOT FCE_BUILD_STATIC AND NOT FCE_BUILD_SHARED)
  message(FATAL_ERROR "At least one of FCE_BUILD_STATIC or FCE_BUILD_SHARED must be ON")
endif()

if(FCE_ENABLE_ZSTD)
  find_path(ZSTD_INCLUDE_DIR zstd.h)
  find_library(ZSTD_LIBRARY NAMES zstd libzstd)
  if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    set(FCE_HAVE_ZSTD ON)
  endif()
endif()

if(NOT WIN32)
  find_package(Threads REQUIRED)
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

function(fce_configure_library target)
  target_include_directories(${target}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
  )
  if(WIN32)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  if(FCE_HAVE_ZSTD)
    target_compile_definitions(${target} PRIVATE FCE_HAVE_ZSTD)
    target_include_directories(${target} PRIVATE ${ZSTD_INCLUDE_DIR})
    target_link_libraries(${target} PRIVATE ${ZSTD_LIBRARY})
  endif()
  if(NOT WIN32)
    target_link_libraries(${target} PRIVATE Threads::Threads)
  endif()
endfunction()

set(FCE_INSTALL_TARGETS)

if(FCE_BUILD_STATIC)
  add_library(fast_cache_engine STATIC
    ${FCE_LIBRARY_SOURCES}
    ${FCE_PUBLIC_HEADERS}
  )
  add_library(fast_cache_engine::fast_cache_engine ALIAS fast_cache_engine)
  fce_configure_library(fast_cache_engine)
  list(APPEND FCE_INSTALL_TARGETS fast_cache_engine)
endif()

if(FCE_BUILD_SHARED)
  add_library(fast_cache_engine_shared SHARED
    ${FCE_LIBRARY_SOURCES}
    ${FCE_PUBLIC_HEADERS}
  )
  add_library(fast_cache_engine::fast_cache_engine_shared ALIAS fast_cache_engine_shared)
  fce_configure_library(fast_cache_engine_shared)
  target_compile_definitions(fast_cache_engine_shared
    PUBLIC FCE_BUILD_SHARED
    PRIVATE FCE_BUILDING_LIBRARY)
  set_target_properties(fast_cache_engine_shared PROPERTIES
    OUTPUT_NAME fast_cache_engine
    ARCHIVE_OUTPUT_NAME fast_cache_engine_shared
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
  list(APPEND FCE_INSTALL_TARGETS fast_cache_engine_shared)
endif()

if(FCE_BUILD_CLI)
  add_executable(fce src/cli/fce_cli.c)
  if(FCE_BUILD_STATIC)
    target_link_libraries(fce PRIVATE fast_cache_engine)
  else()
    target_link_libraries(fce PRIVATE fast_cache_engine_shared)
  endif()
  list(APPEND FCE_INSTALL_TARGETS fce)
endif()

install(TARGETS ${FCE_INSTALL_TARGETS}
  EXPORT fast_cache_engineTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(FCE_INSTALL_CMAKEDIR
  "${CMAKE_INSTALL_LIBDIR}/cmake/fast_cache_engine"
  CACHE STRING "Install directory for Fast Cache Engine CMake package files")

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fast_cache_engineConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/fast_cache_engineConfig.cmake
  INSTALL_DESTINATION ${FCE_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/fast_cache_engineConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(EXPORT fast_cache_engineTargets
  FILE fast_cache_engineTargets.cmake
  NAMESPACE fast_cache_engine::
  DESTINATION ${FCE_INSTALL_CMAKEDIR})

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/fast_cache_engineConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/fast_cache_engineConfigVersion.cmake
  DESTINATION ${FCE_INSTALL_CMAKEDIR})

set(CPACK_PACKAGE_NAME "fast-cache-engine")
set(CPACK_PACKAGE_VENDOR "Fast Cache Engine")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native C library for verifiable, read-optimized persistent key/value caches")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "ZIP;TGZ")
include(CPack)
