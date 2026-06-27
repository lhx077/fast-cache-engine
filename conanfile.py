from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy
import os


class FastCacheEngineConan(ConanFile):
    name = "fast-cache-engine"
    version = "1.0.0"
    license = "Apache-2.0"
    description = "Native C library for verifiable, read-optimized persistent key/value caches"
    topics = ("cache", "key-value", "c", "mmap", "lookup")
    package_type = "library"
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "zstd": [True, False],
        "cli": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "zstd": True,
        "cli": True,
    }
    exports_sources = (
        "CMakeLists.txt",
        "LICENSE",
        "cmake/**",
        "include/**",
        "src/**",
        "python/**",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def requirements(self):
        if self.options.zstd:
            self.requires("zstd/[>=1.5.5 <2]")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.variables["FCE_ENABLE_ZSTD"] = bool(self.options.zstd)
        toolchain.variables["FCE_BUILD_STATIC"] = not bool(self.options.shared)
        toolchain.variables["FCE_BUILD_SHARED"] = bool(self.options.shared)
        toolchain.variables["FCE_BUILD_CLI"] = bool(self.options.cli)
        if self.settings.os != "Windows":
            toolchain.cache_variables["CMAKE_POSITION_INDEPENDENT_CODE"] = bool(self.options.fPIC)
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "fast_cache_engine")
        if self.options.shared:
            self.cpp_info.set_property("cmake_target_name", "fast_cache_engine::fast_cache_engine_shared")
            if self.settings.os == "Windows":
                self.cpp_info.libs = ["fast_cache_engine_shared"]
            else:
                self.cpp_info.libs = ["fast_cache_engine"]
            self.cpp_info.defines.append("FCE_BUILD_SHARED")
        else:
            self.cpp_info.set_property("cmake_target_name", "fast_cache_engine::fast_cache_engine")
            self.cpp_info.libs = ["fast_cache_engine"]
