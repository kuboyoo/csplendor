import os
import platform
import shutil
import subprocess
import sys
import sysconfig

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

try:
    from setuptools.command.bdist_wheel import bdist_wheel
except ImportError:
    from wheel.bdist_wheel import bdist_wheel

SOURCE_ROOT = os.path.abspath(os.path.dirname(__file__))
if SOURCE_ROOT not in sys.path:
    sys.path.insert(0, SOURCE_ROOT)

# PEP 517 executes setup.py without guaranteeing that its directory is on
# sys.path, so the local build helper must be imported after the path fixup.
from _build_support import (  # noqa: E402
    cpu_target_from_environment,
    macos_architectures,
    macos_deployment_target,
    validate_macos_native_build,
    validate_macos_wheel_architectures,
    validate_wheel_build,
)


class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=""):
        Extension.__init__(self, name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)


class CMakeBuild(build_ext):
    def run(self):
        try:
            subprocess.check_output(["cmake", "--version"])
        except OSError as error:
            extensions = ", ".join(extension.name for extension in self.extensions)
            raise RuntimeError(
                f"CMake must be installed to build these extensions: {extensions}"
            ) from error

        for ext in self.extensions:
            self.build_extension(ext)

    def build_extension(self, ext):
        ext_fullpath = os.path.abspath(self.get_ext_fullpath(ext.name))
        extdir = os.path.dirname(ext_fullpath)
        try:
            cpu_target = cpu_target_from_environment()
        except ValueError as error:
            raise RuntimeError(str(error)) from error

        import pybind11

        cfg = "Debug" if self.debug else "Release"
        architectures = None
        if platform.system() == "Darwin":
            try:
                architectures = macos_architectures(
                    python_platform=sysconfig.get_platform(),
                    machine=platform.machine(),
                )
                validate_macos_native_build(
                    cpu_target, architectures, platform.machine()
                )
            except ValueError as error:
                raise RuntimeError(str(error)) from error

        architecture_key = (
            "-".join(architectures) if architectures is not None else "default"
        )
        profile_build_temp = os.path.abspath(
            os.path.join(self.build_temp, cpu_target, architecture_key, cfg.lower())
        )
        profile_output_dir = os.path.join(profile_build_temp, "python-output")

        self.announce(f"csplendor CPU target: {cpu_target}", level=2)
        cmake_args = [
            "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + profile_output_dir,
            "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_{}={}".format(
                cfg.upper(), profile_output_dir
            ),
            "-DPython_EXECUTABLE=" + sys.executable,
            "-DCSPLENDOR_BUILD_PYTHON_MODULE=ON",
            "-DCSPLENDOR_BUILD_NATIVE_TESTS=OFF",
            "-DCSPLENDOR_CPU_TARGET=" + cpu_target,
            "-Dpybind11_DIR=" + pybind11.get_cmake_dir(),
        ]

        build_args = ["--config", cfg]

        if platform.system() == "Windows":
            if sys.maxsize > 2**32:
                cmake_args += ["-A", "x64"]
            build_args += ["--", "/m"]
        else:
            cmake_args += ["-DCMAKE_BUILD_TYPE=" + cfg]
            build_args += ["--", "-j2"]

        if platform.system() == "Darwin":
            try:
                deployment_target = macos_deployment_target(
                    python_target=sysconfig.get_config_var("MACOSX_DEPLOYMENT_TARGET")
                )
            except ValueError as error:
                raise RuntimeError(str(error)) from error
            if deployment_target is not None:
                cmake_args += ["-DCMAKE_OSX_DEPLOYMENT_TARGET=" + deployment_target]
            if architectures is not None:
                cmake_args += ["-DCMAKE_OSX_ARCHITECTURES=" + ";".join(architectures)]

        os.makedirs(profile_output_dir, exist_ok=True)
        os.makedirs(profile_build_temp, exist_ok=True)
        subprocess.check_call(
            ["cmake", ext.sourcedir] + cmake_args, cwd=profile_build_temp
        )
        subprocess.check_call(
            ["cmake", "--build", "."] + build_args, cwd=profile_build_temp
        )

        built_extension = os.path.join(
            profile_output_dir, os.path.basename(ext_fullpath)
        )
        if not os.path.isfile(built_extension):
            raise RuntimeError(
                f"CMake did not produce the expected extension: {built_extension}"
            )
        os.makedirs(extdir, exist_ok=True)
        shutil.copy2(built_extension, ext_fullpath)


class PortableWheel(bdist_wheel):
    _validate_distribution_tag = False

    def run(self):
        try:
            cpu_target = cpu_target_from_environment()
            validate_wheel_build(cpu_target, skip_build=self.skip_build)
        except ValueError as error:
            raise RuntimeError(str(error)) from error

        # Setuptools also asks bdist_wheel for a tag while creating the
        # ephemeral PEP 660 editable wheel. An arm64-only local extension may
        # legitimately be built by a universal2 Python process running as
        # arm64, so enforce distributable tag consistency only for a real
        # bdist_wheel run.
        self._validate_distribution_tag = True
        try:
            if platform.system() == "Darwin":
                self.get_tag()
            super().run()
        finally:
            self._validate_distribution_tag = False

    def get_tag(self):
        tag = super().get_tag()
        if platform.system() != "Darwin" or not self._validate_distribution_tag:
            return tag

        try:
            architectures = macos_architectures(
                python_platform=sysconfig.get_platform(),
                machine=platform.machine(),
            )
            validate_macos_wheel_architectures(architectures, tag[2])
        except ValueError as error:
            raise RuntimeError(str(error)) from error
        return tag


setup(
    ext_modules=[CMakeExtension("csplendor._csplendor")],
    cmdclass={"build_ext": CMakeBuild, "bdist_wheel": PortableWheel},
)
