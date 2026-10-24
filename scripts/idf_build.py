# Build the firmware with ESP-IDF's own build system (CMake + Ninja, what `idf.py build` does)
# instead of PlatformIO's, which only uses CMake to list the sources and compiles them itself with
# SCons.
#
# PlatformIO still installs the packages (ESP-IDF, toolchain, CMake, Ninja) and the lib_deps
# libraries (built as the components in components/), and `pio run -t upload`, `monitor`, `size`
# and IntelliSense work as before. ESP-IDF builds into $BUILD_DIR/idf; the app's .elf / .bin / .map
# are copied to $BUILD_DIR/firmware.* where PlatformIO (upload, the exception decoder) looks.
#
# This replaces env.BuildProgram, so PlatformIO's ESP-IDF builder (espidf.py) never runs.
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

from SCons.Script import AlwaysBuild

Import("env")

platform = env.PioPlatform()
board = env.BoardConfig()
PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
BUILD_DIR = Path(env.subst("$BUILD_DIR"))
IDF_BUILD_DIR = BUILD_DIR / "idf"
IDF_PATH = Path(platform.get_package_dir("framework-espidf"))
PIO_DIR = Path(env.subst("$PROJECT_CORE_DIR"))


def pkg_dir(name):
    return Path(platform.get_package_dir(name))


def idf_version():
    text = (IDF_PATH / "tools" / "cmake" / "version.cmake").read_text()
    return ".".join(re.findall(r"set\(IDF_VERSION_(?:MAJOR|MINOR|PATCH) (\d+)\)", text))


# The Python environment PlatformIO made for this ESP-IDF version (penv/.espidf-<version>).
IDF_VENV = PIO_DIR / "penv" / (".espidf-" + idf_version())
PYTHON = IDF_VENV / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def idf_env():
    e = os.environ.copy()
    e["IDF_PATH"] = str(IDF_PATH)
    e["ESP_ROM_ELF_DIR"] = str(pkg_dir("tool-esp-rom-elfs"))
    e["PATH"] = os.pathsep.join(
        [
            str(pkg_dir("toolchain-xtensa-esp-elf") / "bin"),
            str(pkg_dir("tool-ninja")),
            str(pkg_dir("tool-cmake") / "bin"),
            str(PYTHON.parent),
            e["PATH"],
        ]
    )
    return e


def run(cmd):
    print(" ".join(str(c) for c in cmd))
    if subprocess.run([str(c) for c in cmd], env=idf_env()).returncode:
        env.Exit(1)


def ensure_python():
    if not PYTHON.is_file():
        sys.stderr.write(
            "Error: ESP-IDF's Python environment %s is missing. PlatformIO's ESP-IDF builder "
            "creates it; this build doesn't run that builder.\n" % IDF_VENV
        )
        env.Exit(1)
    # ESP-IDF's build calls esptool (bootloader and app images), which PlatformIO keeps in its own
    # package rather than in this environment: install that package into it once.
    if subprocess.run([str(PYTHON), "-c", "import esptool"], capture_output=True).returncode:
        uv = PIO_DIR / "penv" / ("Scripts/uv.exe" if os.name == "nt" else "bin/uv")
        run([uv, "pip", "install", "--python", PYTHON, pkg_dir("tool-esptoolpy")])


def ensure_configured():
    """Run CMake's configure step when there is no build yet. After that Ninja reconfigures by
    itself when a CMakeLists.txt, sdkconfig or the git commit (.git/HEAD - the firmware's version
    string is `git describe`) changes."""
    if (IDF_BUILD_DIR / "build.ninja").is_file():
        return
    ensure_python()
    run(
        [
            pkg_dir("tool-cmake") / "bin" / "cmake",
            "-S", PROJECT_DIR,
            "-B", IDF_BUILD_DIR,
            "-G", "Ninja",
            "-DIDF_TARGET=" + board.get("build.mcu"),
            "-DPYTHON=" + str(PYTHON),
            "-DPYTHON_DEPS_CHECKED=1",
        ]
    )


def project_description():
    return json.loads((IDF_BUILD_DIR / "project_description.json").read_text())


def build_idf(target, source, env):
    ensure_configured()
    run([pkg_dir("tool-cmake") / "bin" / "cmake", "--build", IDF_BUILD_DIR])
    app_elf = IDF_BUILD_DIR / project_description()["app_elf"]
    shutil.copyfile(app_elf, str(target[0]))
    shutil.copyfile(app_elf.with_suffix(".map"), BUILD_DIR / (env.subst("$PROGNAME") + ".map"))


def copy_app_bin(target, source, env):
    flasher = json.loads((IDF_BUILD_DIR / "flasher_args.json").read_text())
    shutil.copyfile(IDF_BUILD_DIR / flasher["app"]["file"], str(target[0]))


def use_compile_commands():
    """Include paths, defines and language flags of the project component, for IntelliSense."""
    commands = json.loads((IDF_BUILD_DIR / "compile_commands.json").read_text())
    src_dir = (PROJECT_DIR / "src").resolve()
    for suffix, flags_var in ((".cpp", "CXXFLAGS"), (".c", "CFLAGS")):
        entry = next(
            c
            for c in commands
            if Path(c["file"]).suffix == suffix and Path(c["file"]).resolve().parent == src_dir
        )
        for arg in shlex.split(entry["command"].replace("\\", "/"), posix=False)[1:]:
            if arg.startswith("-I"):
                env.AppendUnique(CPPPATH=[arg[2:]])
            elif arg.startswith("-D"):
                name, _, value = arg[2:].replace('/"', '"').partition("=")
                env.AppendUnique(CPPDEFINES=[(name, value) if value else name])
            elif arg.startswith("-std="):
                env.Append(**{flags_var: [arg]})


def build_program(env):
    ensure_configured()

    if env.IsIntegrationDump():
        use_compile_commands()

    # Flash what ESP-IDF's build lays out: bootloader, partition table, OTA data, then the app.
    flasher = json.loads((IDF_BUILD_DIR / "flasher_args.json").read_text())
    app_offset = flasher["app"]["offset"]
    env.Replace(
        ESP32_APP_OFFSET=app_offset,
        FLASH_EXTRA_IMAGES=[
            (offset, str(IDF_BUILD_DIR / file))
            for offset, file in flasher["flash_files"].items()
            if offset != app_offset
        ],
    )
    env["INTEGRATION_EXTRA_DATA"].update({"application_offset": app_offset})

    # The platform turns the .elf into firmware.bin with ElfToBin; ESP-IDF already made it.
    env["BUILDERS"]["ElfToBin"].action = env.VerboseAction(copy_app_bin, "Building $TARGET")

    program = env.Command(
        "$BUILD_DIR/${PROGNAME}.elf",
        [],
        env.VerboseAction(build_idf, "Building with ESP-IDF (CMake + Ninja)"),
    )
    AlwaysBuild(program)  # Ninja decides what is out of date
    env.Replace(PIOMAINPROG=program)
    AlwaysBuild(
        env.Alias(
            "checkprogsize",
            program,
            env.VerboseAction(env.CheckUploadSize, "Checking size $PIOMAINPROG"),
        )
    )
    return program


env.AddMethod(build_program, "BuildProgram")
