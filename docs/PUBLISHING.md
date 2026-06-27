# 发布到包管理器

本文说明如何把本项目发布到常见包管理器。它刻意独立于自述文件：普通用户只需要看到安装和构建文档，维护者发布流程不放进自述文件。

## 发布前检查

先运行本地发布检查：

```text
cmake -S . -B build -DFCE_ENABLE_ZSTD=OFF
cmake --build build --config Release --parallel 2
cmake --build build --target package --config Release
python -m pip wheel . --no-deps -w dist
```

确认工作树里只有有意提交的源码和配置变更：

```text
git status --short --untracked-files=all
```

不要提交生成产物，例如 `build/`、`dist/`、`wheelhouse/`、压缩包、Conan 本地缓存、CPack 临时目录等。

## 版本号同步

发布前需要同步这些版本号：

- `CMakeLists.txt` 里的 `project(... VERSION ...)`
- `src/core/fce_version.c` 里的 `fce_version_string()`
- `vcpkg.json` 里的 `version`
- `conanfile.py` 里的 `version`
- `pyproject.toml` 里的 `version`

如果希望自动化流程创建发布页并上传三平台产物，发布提交的标题第一行需要包含 `[Release]`：

```text
[Release] v1.0.0
```

## CMake 和 CPack 压缩包

CMake 安装、导出和 CPack 配置属于本仓库，应该提交在这里。

生成压缩包：

```text
cmake -S . -B build -DFCE_ENABLE_ZSTD=OFF
cmake --build build --config Release
cmake --build build --target package --config Release
```

生成的 `.zip` 和 `.tar.gz` 可以上传到项目发布页。它们是构建产物，不要提交到仓库。

## Conan

本仓库自带的 Conan 配方文件是根目录的 `conanfile.py`。这是项目自维护配方文件的常见位置，应该提交到本仓库。

本地验证：

```text
conan profile detect --force
conan create . --build=missing -o fast-cache-engine/*:zstd=False
```

发布到某个 Conan 远端：

```text
conan remote add <remote-name> <remote-url>
conan upload "fast-cache-engine/1.0.0" -r <remote-name> --confirm
```

如果要发布到 ConanCenter，不是直接提交本仓库，而是给 `conan-io/conan-center-index` 开合并请求。配方文件通常放在：

```text
recipes/fast-cache-engine/all/
```

ConanCenter 的配方文件可以参考本仓库的 `conanfile.py`，但 ConanCenter 有自己的格式、元数据、测试包和版本目录要求。

## vcpkg

本仓库中的 `vcpkg.json` 是上游项目清单，应该保留在本仓库。它用于说明可选依赖，并支持用户用 vcpkg 清单模式构建项目。

使用本地 vcpkg 目录测试：

```text
vcpkg install --feature-flags=manifests
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

如果要发布到官方 vcpkg 注册表，需要给 `microsoft/vcpkg` 开合并请求。移植包文件应该放在 vcpkg 仓库，不要放在本仓库：

```text
ports/fast-cache-engine/portfile.cmake
ports/fast-cache-engine/vcpkg.json
versions/f-/fast-cache-engine.json
```

官方 vcpkg 移植包通常会拉取某个 GitHub 标签的源码包，校验 SHA512，运行 CMake 配置和安装步骤，并按 vcpkg 规范清理不需要的调试文件和重复头文件。

## PyPI

Python 打包由本仓库的 `pyproject.toml` 和 `setup.py` 维护。当前 Python 绑定是 `ctypes` 封装，所以 wheel 包必须把本机共享库放在 Python 包目录旁边。

安装发布工具：

```text
python -m pip install --upgrade build twine cibuildwheel
```

本地构建 wheel 包：

```text
python -m build
```

检查并测试：

```text
python -m twine check dist/*
python -m pip install --force-reinstall --no-deps dist/fast_cache_engine-*.whl
python -c "import fast_cache_engine; print(fast_cache_engine.version_string())"
```

先上传到 TestPyPI：

```text
python -m twine upload --repository testpypi dist/*
```

确认无误后再上传到 PyPI：

```text
python -m twine upload dist/*
```

跨平台 wheel 包应该用持续集成里的 `cibuildwheel` 构建，因为每个平台的 wheel 包都要包含对应平台的本机库。

## Homebrew

Homebrew 公式文件通常不提交到本仓库，除非你在同一组织下维护一个独立 tap 仓库。常见做法是给 tap 仓库开合并请求，例如：

```text
homebrew-core/Formula/fast-cache-engine.rb
```

或者私有 tap：

```text
homebrew-fast-cache-engine/Formula/fast-cache-engine.rb
```

公式文件应该拉取某个标签的源码包，校验 SHA256，用 CMake 构建，运行一个小型冒烟测试，并安装头文件、库文件和 `fce` 命令行程序。

## 哪些内容应该提交到本仓库

这些文件应该保留在本仓库：

- `CMakeLists.txt`
- `cmake/fast_cache_engineConfig.cmake.in`
- `conanfile.py`
- `vcpkg.json`
- `pyproject.toml`
- `setup.py`
- `docs/PACKAGING.md`
- `docs/PACKAGING.zh.md`
- `docs/PUBLISHING.md`

这些内容不要提交：

- `build/`
- `build_*/`
- `build-wheel/`
- `dist/`
- `wheelhouse/`
- `_CPack_Packages/`
- 生成的 `.zip`、`.tar.gz`、`.whl`
- Conan 本地缓存目录
- vcpkg 注册表生成的版本文件
