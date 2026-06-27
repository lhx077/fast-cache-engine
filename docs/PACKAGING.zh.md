# 打包说明

本仓库只提交对上游用户和 CI 有用的正式打包配置。构建产物、压缩包、wheel、Conan 本地缓存等生成文件都应保持未跟踪。

## CMake Install Package

Fast Cache Engine 会安装可被 `find_package` 使用的 CMake package：

```text
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/path/to/prefix
cmake --build build --config Release
cmake --install build --config Release
```

消费端使用：

```cmake
find_package(fast_cache_engine CONFIG REQUIRED)
target_link_libraries(app PRIVATE fast_cache_engine::fast_cache_engine)
```

如果安装了 shared target，也可以链接：

```cmake
target_link_libraries(app PRIVATE fast_cache_engine::fast_cache_engine_shared)
```

## CPack

CPack 已在 CMake 中配置：

```text
cmake -S . -B build
cmake --build build --config Release
cmake --build build --target package --config Release
```

生成的 package archive 属于构建产物，不应提交。

## Conan

Conan recipe 放在仓库根目录 `conanfile.py`，这是项目自带 recipe 的常见位置。

本地创建 package：

```text
conan create . --build=missing -o fast-cache-engine/*:zstd=False
```

recipe 会把 Conan option 映射到 CMake option：

- `shared`：选择 shared 或 static library variant。
- `zstd`：启用或关闭可选 zstd codec。
- `cli`：是否包含 `fce` 命令行工具。

## vcpkg

仓库根目录包含 `vcpkg.json`，作为上游项目 manifest。用户可以用它通过 vcpkg 安装 zstd 等可选依赖，再用 CMake 构建项目。

官方 vcpkg registry port 通常应提交到 vcpkg registry 仓库，而不是放在上游项目里。如果需要私有 registry，应把 port 保存在该 registry 中，不要把生成的 registry 文件混入本源码树。

## Python Wheels

Python packaging 由 `pyproject.toml` 和 `setup.py` 配置。当前 Python binding 是 `ctypes` wrapper，因此 wheel 会把 native shared library 放到 Python package 目录旁边。

自定义 setuptools build command 会用 `FCE_BUILD_SHARED=ON`、`FCE_BUILD_STATIC=OFF`、`FCE_BUILD_CLI=OFF` 运行 CMake，然后把平台对应的 shared library 复制进 wheel。

本地 wheel 构建：

```text
python -m build
```

跨平台 wheel 建议在 CI 中用 cibuildwheel 构建。`dist/`、`wheelhouse/` 等输出目录已加入 `.gitignore`。
