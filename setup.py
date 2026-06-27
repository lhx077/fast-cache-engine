from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py as _build_py
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


class build_py(_build_py):
    def run(self) -> None:
        super().run()
        if os.environ.get("FCE_PYTHON_SKIP_NATIVE_BUILD") == "1":
            return
        self._build_native_library()

    def _build_native_library(self) -> None:
        root = Path(__file__).resolve().parent
        build_cmd = self.get_finalized_command("build")
        build_temp = Path(build_cmd.build_temp).resolve() / "fast_cache_engine_native"
        package_dir = Path(self.build_lib).resolve() / "fast_cache_engine"
        package_dir.mkdir(parents=True, exist_ok=True)

        cmake = shutil.which("cmake")
        if not cmake:
            raise RuntimeError("cmake is required to build the fast-cache-engine wheel")

        enable_zstd = os.environ.get("FCE_PYTHON_ENABLE_ZSTD", "OFF")
        configure = [
            cmake,
            "-S",
            str(root),
            "-B",
            str(build_temp),
            f"-DFCE_ENABLE_ZSTD={enable_zstd}",
            "-DFCE_BUILD_STATIC=OFF",
            "-DFCE_BUILD_SHARED=ON",
            "-DFCE_BUILD_CLI=OFF",
        ]
        subprocess.check_call(configure)
        subprocess.check_call([
            cmake,
            "--build",
            str(build_temp),
            "--config",
            "Release",
            "--parallel",
            "2",
        ])

        candidates = [
            build_temp / "Release" / "fast_cache_engine.dll",
            build_temp / "Debug" / "fast_cache_engine.dll",
            build_temp / "fast_cache_engine.dll",
            build_temp / "libfast_cache_engine.so",
            build_temp / "libfast_cache_engine.dylib",
        ]
        for src in candidates:
            if src.exists():
                shutil.copy2(src, package_dir / src.name)
                return
        raise RuntimeError(f"native fast-cache-engine shared library was not found in {build_temp}")


class bdist_wheel(_bdist_wheel):
    def finalize_options(self) -> None:
        super().finalize_options()
        self.root_is_pure = False


setup(cmdclass={"build_py": build_py, "bdist_wheel": bdist_wheel})
