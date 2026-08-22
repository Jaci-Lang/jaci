# ADR 0019: Cross-Platform Toolchains, Direct Binary Bundling, and Windowed Subsystems

## Context

Single-binary compilation in Jaci previously relied on external GNU/MinGW toolchains (`g++`, `x86_64-w64-mingw32-g++`) and Unix linker flags (`-Wl,--start-group`), requiring developers to have full C++ cross-compilation toolchains installed. Furthermore, Windows desktop applications required support for the GUI subsystem (`WinMain` entry point, `/SUBSYSTEM:WINDOWS`, `-mwindows`) to run without an attached console window popup ("Main Window" mode).

## Decision

1. **Zero-Toolchain Direct Binary Bundling (`--direct` / `BundleMode::Direct`)**:
   - Introduced a self-contained packaging format: `[Base Executable (e.g. luau)] + [Serialized JACI Payload (bytecode, assets, configuration)] + [24-byte Trailer]`.
   - Standalone binaries can now be packaged immediately on any platform (Windows, Linux, macOS) without requiring MinGW, GCC, Clang, or any external C/C++ compiler toolchain.
   - At startup, `SingleBinaryCompiler::checkAndRunBundledPayload` parses the magic trailer (`"JACIPKG\0"`), loads the in-memory payload into the VM, injects virtual filesystem hooks (`fs.readFile`, `fs.exists`, `fs.stat`), and executes the entry module with native CodeGen support.

2. **Native Toolchain Interoperability**:
   - Added support for MSVC (`cl.exe`), Clang-cl, Apple Clang (`clang++ -target ...`), GCC, and Zig (`zig c++`).
   - Removed GNU-specific linker flag requirements on macOS and Windows MSVC.
   - Cross-platform temp directory resolution using `GetTempPathA` on Windows and `TMPDIR`/`/tmp` on POSIX.

3. **Windows GUI / Main Window Mode (`--windowed`, `--gui`, `-W`)**:
   - Implemented `WinMain` entry points and command-line argument decoders in both direct runners and native generated C++ stubs.
   - Added `/SUBSYSTEM:WINDOWS` (MSVC) and `-mwindows` (MinGW) flags to suppress console popups for windowed desktop applications.

## Consequences

- Compiling standalone executables now works out-of-the-box on any system with zero compiler dependencies.
- Full compatibility with native platform toolchains (MSVC on Windows, Xcode/Clang on macOS, GCC on Linux).
- Full support for GUI/windowed desktop applications on Windows.

## Copyright

Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio under the MIT License (see `LICENSE.txt`).
