# Packaging

This repository keeps packaging files that are useful to upstream users and CI. Generated package outputs are ignored and should not be committed.

## CMake Install Package

Fast Cache Engine installs an exportable CMake package:

```text
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/path/to/prefix
cmake --build build --config Release
cmake --install build --config Release
```

Consumers can use:

```cmake
find_package(fast_cache_engine CONFIG REQUIRED)
target_link_libraries(app PRIVATE fast_cache_engine::fast_cache_engine)
```

When the shared target is installed, consumers may link:

```cmake
target_link_libraries(app PRIVATE fast_cache_engine::fast_cache_engine_shared)
```

## CPack

CPack is configured for source-tree builds:

```text
cmake -S . -B build
cmake --build build --config Release
cmake --build build --target package --config Release
```

Package archives are build outputs and should remain untracked.

## Conan

The upstream Conan recipe lives at the repository root as `conanfile.py`, which is the usual location for a project-owned recipe.

Example local package creation:

```text
conan create . --build=missing -o fast-cache-engine/*:zstd=False
```

The recipe maps Conan options to CMake options:

- `shared`: builds either the shared or static library variant.
- `zstd`: enables or disables the optional zstd codec.
- `cli`: includes or excludes the `fce` command line tool.

## vcpkg

The repository includes `vcpkg.json` as an upstream manifest. This lets users install optional dependencies, such as zstd, through vcpkg while building the project with CMake.

Official vcpkg registry ports are normally submitted to the vcpkg registry repository, not stored in the upstream project. If a private registry is needed, keep the port in that registry rather than mixing generated registry files into this source tree.

## Python Wheels

Python packaging is configured through `pyproject.toml` and `setup.py`. The Python binding is a `ctypes` wrapper and wheels include the native shared library next to the Python package.

The custom setuptools build command runs CMake with `FCE_BUILD_SHARED=ON`, `FCE_BUILD_STATIC=OFF`, and `FCE_BUILD_CLI=OFF`, then copies the platform shared library into the wheel.

Local wheel build:

```text
python -m build
```

Cross-platform wheels should be built in CI with cibuildwheel. Wheel outputs such as `dist/` and `wheelhouse/` are ignored.
