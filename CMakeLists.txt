# This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
if(EXT_PLATFORM_STRING)
    include(EXTLuau.cmake)
    return()
endif()

cmake_minimum_required(VERSION 3.10)

option(LUAU_BUILD_CLI "Build CLI" ON)
option(LUAU_BUILD_TESTS "Build tests" ON)
option(LUAU_BUILD_WEB "Build Web module" OFF)
option(LUAU_WERROR "Warnings as errors" OFF)
option(LUAU_STATIC_CRT "Link with the static CRT (/MT)" OFF)
option(LUAU_EXTERN_C "Use extern C for all APIs" OFF)

# Version string embedded in the CLI binaries, reported by `luau --version`.
# Set from the release workflow; defaults to "dev" for local builds.
if(NOT DEFINED JACI_VERSION)
    set(JACI_VERSION "dev" CACHE STRING "Jaci version string embedded in CLI binaries")
endif()
option(LUAU_BUILD_SHARED "Build as a shared library" OFF)
option(LUAU_ENABLE_LTO "Enable Link-Time Optimization (LTO / IPO)" OFF)
option(LUAU_OPTIMIZE_SIZE "Optimize compilation and linking for minimal binary size" OFF)
option(LUAU_STRIP_BINARIES "Strip debug symbols from release executable artifacts" OFF)
# LLVM JIT backend: Jaci requires LLVM by default; OFF remains available only
# for explicit compatibility builds.
set(LUAU_USE_LLVM "ON" CACHE STRING "Use LLVM as the native code generation backend (AUTO/ON/OFF)")
set_property(CACHE LUAU_USE_LLVM PROPERTY STRINGS AUTO ON OFF)

if(LUAU_BUILD_SHARED AND NOT LUAU_EXTERN_C)
    message(FATAL_ERROR "LUAU_BUILD_SHARED requires LUAU_EXTERN_C to be ON")
endif()

cmake_policy(SET CMP0054 NEW)
cmake_policy(SET CMP0091 NEW)
if(POLICY CMP0069)
    cmake_policy(SET CMP0069 NEW)
endif()

if(LUAU_STATIC_CRT)
    cmake_minimum_required(VERSION 3.15)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

project(Luau LANGUAGES CXX C)

set(JACI_LOGO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/JaciLogoData.h")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/assets/ascii.txt -DOUTPUT=${JACI_LOGO_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateJaciLogo.cmake
    RESULT_VARIABLE JACI_LOGO_RESULT
)
if(NOT JACI_LOGO_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to embed assets/ascii.txt")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/ascii.txt")

if (LUAU_BUILD_SHARED)
    add_library(Luau.Common SHARED)
    add_library(Luau.CLI.lib SHARED)
    add_library(Luau.Ast SHARED)
    add_library(Luau.Bytecode SHARED)
    add_library(Luau.Inliner SHARED)
    add_library(Luau.Compiler SHARED)
    add_library(Luau.Config SHARED)
    add_library(Luau.Analysis SHARED)
    add_library(Luau.CodeGen SHARED)
    add_library(Luau.VM SHARED)
    add_library(Luau.Require SHARED)
    add_library(isocline SHARED)
else()
    add_library(Luau.Common STATIC)
    add_library(Luau.CLI.lib STATIC)
    add_library(Luau.Ast STATIC)
    add_library(Luau.Bytecode STATIC)
    add_library(Luau.Inliner STATIC)
    add_library(Luau.Compiler STATIC)
    add_library(Luau.Config STATIC)
    add_library(Luau.Analysis STATIC)
    add_library(Luau.CodeGen STATIC)
    add_library(Luau.VM STATIC)
    add_library(Luau.Require STATIC)
    add_library(isocline STATIC)
endif()

if(LUAU_BUILD_CLI)
    add_executable(Luau.Repl.CLI)
    add_executable(Luau.Analyze.CLI)
    add_executable(Luau.Ast.CLI)
    add_executable(Luau.Reduce.CLI)
    add_executable(Luau.Compile.CLI)
    add_executable(Luau.Bytecode.CLI)

    # This also adds target `name` on Linux/macOS and `name.exe` on Windows
    set_target_properties(Luau.Repl.CLI PROPERTIES OUTPUT_NAME luau)
    set_target_properties(Luau.Analyze.CLI PROPERTIES OUTPUT_NAME luau-analyze)
    set_target_properties(Luau.Ast.CLI PROPERTIES OUTPUT_NAME luau-ast)
    set_target_properties(Luau.Reduce.CLI PROPERTIES OUTPUT_NAME luau-reduce)
    set_target_properties(Luau.Compile.CLI PROPERTIES OUTPUT_NAME luau-compile)
    set_target_properties(Luau.Bytecode.CLI PROPERTIES OUTPUT_NAME luau-bytecode)

    target_compile_definitions(Luau.Repl.CLI PRIVATE JACI_VERSION="${JACI_VERSION}")
    target_include_directories(Luau.Repl.CLI PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

if(LUAU_BUILD_TESTS)
    add_executable(Luau.UnitTest)
    add_executable(Luau.Conformance)
    add_executable(Luau.CLI.Test)
    add_executable(t_native_interop_test)
    target_include_directories(Luau.CLI.Test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

if(LUAU_BUILD_WEB)
    add_executable(Luau.Web)
endif()

# Proxy target to make it possible to depend on private VM headers
add_library(Luau.VM.Internals INTERFACE)

include(Sources.cmake)

target_compile_features(Luau.Common PUBLIC cxx_std_17)
target_include_directories(Luau.Common PUBLIC Common/include)

target_compile_features(Luau.CLI.lib PUBLIC cxx_std_17)
target_include_directories(Luau.CLI.lib PUBLIC CLI/include Analysis/include)
target_link_libraries(Luau.CLI.lib PUBLIC Luau.Common Luau.Config Luau.Compiler Luau.VM Luau.Require Luau.CodeGen)

target_compile_features(Luau.Ast PUBLIC cxx_std_17)
target_include_directories(Luau.Ast PUBLIC Ast/include)
target_link_libraries(Luau.Ast PUBLIC Luau.Common)

target_compile_features(Luau.Bytecode PUBLIC cxx_std_17)
target_include_directories(Luau.Bytecode PUBLIC Bytecode/include)
target_link_libraries(Luau.Bytecode PUBLIC Luau.Common)

target_compile_features(Luau.Inliner PUBLIC cxx_std_17)
target_include_directories(Luau.Inliner PUBLIC Inliner/include)
target_include_directories(Luau.Inliner PRIVATE Bytecode/src)
target_link_libraries(Luau.Inliner PRIVATE Luau.VM Luau.VM.Internals)
target_link_libraries(Luau.Inliner PUBLIC Luau.Bytecode)

target_compile_features(Luau.Compiler PUBLIC cxx_std_17)
target_include_directories(Luau.Compiler PUBLIC Compiler/include)
target_link_libraries(Luau.Compiler PUBLIC Luau.Ast Luau.Bytecode)

target_compile_features(Luau.Config PUBLIC cxx_std_17)
target_include_directories(Luau.Config PUBLIC Config/include)
target_link_libraries(Luau.Config PUBLIC Luau.Ast)
target_link_libraries(Luau.Config PRIVATE Luau.Compiler Luau.VM)

target_compile_features(Luau.Analysis PUBLIC cxx_std_17)
target_include_directories(Luau.Analysis PUBLIC Analysis/include)
target_link_libraries(Luau.Analysis PUBLIC Luau.Ast Luau.Config)
target_link_libraries(Luau.Analysis PRIVATE Luau.Compiler Luau.VM)

target_compile_features(Luau.CodeGen PRIVATE cxx_std_17)
target_include_directories(Luau.CodeGen PUBLIC CodeGen/include)
target_link_libraries(Luau.CodeGen PRIVATE Luau.VM Luau.VM.Internals) # Code generation needs VM internals
target_link_libraries(Luau.CodeGen PUBLIC Luau.Common)

# LLVM JIT backend: link the LLVM development libraries into CodeGen when found or required
if(LUAU_USE_LLVM STREQUAL "ON" OR LUAU_USE_LLVM STREQUAL "AUTO")
    find_program(LLVM_CONFIG_EXECUTABLE NAMES llvm-config llvm-config-22 llvm-config-21 llvm-config-19 llvm-config-18)

    if(LLVM_CONFIG_EXECUTABLE)
        execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --cmakedir OUTPUT_VARIABLE LLVM_CMAKE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
        file(TO_CMAKE_PATH "${LLVM_CMAKE_DIR}" LLVM_CMAKE_DIR)
    endif()

    if(LLVM_CMAKE_DIR AND EXISTS "${LLVM_CMAKE_DIR}/LLVMConfig.cmake")
        find_package(LLVM CONFIG PATHS "${LLVM_CMAKE_DIR}" NO_DEFAULT_PATH QUIET)
    endif()

    if(NOT LLVM_FOUND)
        find_package(LLVM CONFIG QUIET)
    endif()
endif()

if(LLVM_FOUND)
    message(STATUS "LLVM JIT backend: using LLVM ${LLVM_PACKAGE_VERSION}")
    target_compile_definitions(Luau.CodeGen PUBLIC LUAU_USE_LLVM=1)
    target_include_directories(Luau.CodeGen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    if(MSVC)
        # LLVM 19 JSON templates trigger C4244 inside the MSVC standard library.
        set_source_files_properties(CodeGen/src/JitObjectLoader.cpp PROPERTIES COMPILE_OPTIONS /wd4244)
    endif()
    # Prefer the monolithic shared library (the per-component static archives
    # are not always usable on distro packages).
    if(TARGET LLVM)
        target_link_libraries(Luau.CodeGen PRIVATE LLVM)
    else()
        find_library(LLVM_MONOLITHIC_LIBRARY
            NAMES LLVM-${LLVM_VERSION_MAJOR} LLVM
            HINTS ${LLVM_LIBRARY_DIRS}
            NO_DEFAULT_PATH
        )
        if(LLVM_MONOLITHIC_LIBRARY)
            target_link_libraries(Luau.CodeGen PRIVATE ${LLVM_MONOLITHIC_LIBRARY})
        else()
            set(LLVM_COMPONENT_LIBS ${LLVM_AVAILABLE_LIBS})
            list(FILTER LLVM_COMPONENT_LIBS EXCLUDE REGEX "^LLVM-C$")
            target_link_libraries(Luau.CodeGen PRIVATE ${LLVM_COMPONENT_LIBS})
        endif()
    endif()
elseif(LUAU_USE_LLVM STREQUAL "ON")
    message(FATAL_ERROR "LUAU_USE_LLVM=ON but no LLVM development package was found")
elseif(LUAU_USE_LLVM STREQUAL "AUTO")
    message(STATUS "LLVM JIT backend: no LLVM development package found; building without it")
endif()

target_compile_features(Luau.VM PRIVATE cxx_std_17)
target_include_directories(Luau.VM PUBLIC VM/include)
target_link_libraries(Luau.VM PUBLIC Luau.Common)

# FFI library requires libdl on POSIX for dlopen/dlsym
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
    target_link_libraries(Luau.VM PRIVATE dl)
endif()

# Net library requires Winsock on Windows
if(WIN32)
    target_link_libraries(Luau.VM PRIVATE ws2_32)
endif()

target_compile_features(Luau.Require PUBLIC cxx_std_17)
target_include_directories(Luau.Require PUBLIC Require/include)
target_link_libraries(Luau.Require PUBLIC Luau.Config Luau.VM)

target_include_directories(isocline PUBLIC extern/isocline/include)

target_include_directories(Luau.VM.Internals INTERFACE VM/src)

set(LUAU_OPTIONS)
set(LUAU_LINK_OPTIONS)

if(MSVC)
    list(APPEND LUAU_OPTIONS /D_CRT_SECURE_NO_WARNINGS) # We need to use the portable CRT functions.
    list(APPEND LUAU_OPTIONS "/we4018") # Signed/unsigned mismatch
    list(APPEND LUAU_OPTIONS "/we4388") # Also signed/unsigned mismatch
    list(APPEND LUAU_OPTIONS /Gy) # Enable function-level linking
    list(APPEND LUAU_OPTIONS /Zc:inline) # Remove unreferenced functions and data
    list(APPEND LUAU_OPTIONS /Z7) # Store debug info in .obj (much faster for parallel builds)
    list(APPEND LUAU_OPTIONS /GR-) # Disable RTTI for compact binaries
    list(APPEND LUAU_OPTIONS /EHsc) # Enable C++ exceptions
    # Only use /OPT:REF and /OPT:ICF in release builds, as they disable incremental linking and drastically slow down the linker
    list(APPEND LUAU_LINK_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF>")
    list(APPEND LUAU_LINK_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>")
else()
    list(APPEND LUAU_OPTIONS -Wall) # All warnings
    list(APPEND LUAU_OPTIONS -Wimplicit-fallthrough)
    list(APPEND LUAU_OPTIONS -Wsign-compare) # This looks to be included in -Wall for GCC but not clang
    list(APPEND LUAU_OPTIONS -ffunction-sections)
    list(APPEND LUAU_OPTIONS -fdata-sections)
    list(APPEND LUAU_OPTIONS "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
    if(NOT LUAU_BUILD_SHARED)
        list(APPEND LUAU_OPTIONS -fvisibility=hidden)
        list(APPEND LUAU_OPTIONS "$<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>")
    endif()

    if(APPLE)
        list(APPEND LUAU_LINK_OPTIONS -Wl,-dead_strip)
    elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|Android|BSD|SunOS")
        list(APPEND LUAU_LINK_OPTIONS -Wl,--gc-sections)
    endif()
endif()

if(LUAU_OPTIMIZE_SIZE)
    if(MSVC)
        list(APPEND LUAU_OPTIONS /O1 /Os)
    else()
        list(APPEND LUAU_OPTIONS -Os)
    endif()
endif()

if(LUAU_STRIP_BINARIES AND NOT MSVC AND NOT APPLE)
    list(APPEND LUAU_LINK_OPTIONS -s)
endif()

if(LUAU_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "IPO / LTO not supported: ${ipo_error}")
    endif()
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    list(APPEND LUAU_OPTIONS /MP) # Distribute single project compilation across multiple cores
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Some gcc versions treat var in `if (type var = val)` as unused
    # Some gcc versions treat variables used in constexpr if blocks as unused
    list(APPEND LUAU_OPTIONS -Wno-unused)
    # some gcc versions warn maybe uninitialized on optional<string> members on structs
    list(APPEND LUAU_OPTIONS -Wno-maybe-uninitialized)
    # some gcc versions report valid vector storage as a non-heap allocation after inlining
    list(APPEND LUAU_OPTIONS -Wno-free-nonheap-object)
endif()

# Enabled in CI; we should be warning free on our main compiler versions but don't guarantee being warning free everywhere
if(LUAU_WERROR)
    if(MSVC)
        list(APPEND LUAU_OPTIONS /WX) # Warnings are errors
    else()
        list(APPEND LUAU_OPTIONS -Werror) # Warnings are errors
    endif()
endif()

if(LUAU_BUILD_WEB)
    # add -fexceptions for emscripten to allow exceptions to be caught in C++
    list(APPEND LUAU_OPTIONS -fexceptions)
endif()

set(ISOCLINE_OPTIONS)

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    list(APPEND ISOCLINE_OPTIONS -Wno-unused-function)
endif()

target_compile_options(Luau.Common PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.Ast PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.Bytecode PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.Inliner PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.Compiler PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.Config PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.Analysis PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.CodeGen PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.VM PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.Require PRIVATE ${LUAU_OPTIONS})
target_compile_options(Luau.CLI.lib PRIVATE ${LUAU_OPTIONS})
target_compile_options(isocline PRIVATE ${LUAU_OPTIONS} ${ISOCLINE_OPTIONS})

if(LUAU_EXTERN_C)
    # enable extern "C" for VM (lua.h, lualib.h) and Compiler (luacode.h) to make Luau friendlier to use from non-C++ languages
    # note that we enable LUA_USE_LONGJMP=1 as well; otherwise functions like luaL_error will throw C++ exceptions, which can't be done from extern "C" functions
    target_compile_definitions(Luau.VM PUBLIC LUA_USE_LONGJMP=1)

    if(LUAU_BUILD_SHARED)
        if(MSVC)
            target_compile_definitions(Luau.VM PUBLIC "LUA_API=extern \"C\" __declspec(dllexport)")
            target_compile_definitions(Luau.Compiler PUBLIC "LUACODE_API=extern \"C\" __declspec(dllexport)")
            target_compile_definitions(Luau.CodeGen PUBLIC "LUACODEGEN_API=extern \"C\" __declspec(dllexport)")
        else()
            # GCC/Clang
            target_compile_definitions(Luau.VM PUBLIC "LUA_API=extern \"C\" __attribute__((visibility(\"default\")))")
            target_compile_definitions(Luau.Compiler PUBLIC "LUACODE_API=extern \"C\" __attribute__((visibility(\"default\")))")
            target_compile_definitions(Luau.CodeGen PUBLIC "LUACODEGEN_API=extern \"C\" __attribute__((visibility(\"default\")))")
        endif()
    else()
        # For static builds (or if not exporting) just use extern "C"
        target_compile_definitions(Luau.VM PUBLIC "LUA_API=extern \"C\"")
        target_compile_definitions(Luau.Compiler PUBLIC "LUACODE_API=extern \"C\"")
        target_compile_definitions(Luau.CodeGen PUBLIC "LUACODEGEN_API=extern \"C\"")
    endif()
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND MSVC_VERSION GREATER_EQUAL 1924)
    # disable partial redundancy elimination which regresses interpreter codegen substantially in VS2022:
    # https://developercommunity.visualstudio.com/t/performance-regression-on-a-complex-interpreter-lo/1631863
    set_source_files_properties(VM/src/lvmexecute.cpp PROPERTIES COMPILE_FLAGS /d2ssa-pre-)
endif()

if (NOT MSVC)
    # disable support for math_errno which allows compilers to lower sqrt() into a single CPU instruction
    target_compile_options(Luau.VM PRIVATE -fno-math-errno)
endif()

if(MSVC AND LUAU_BUILD_CLI)
    # the default stack size that MSVC linker uses is 1 MB; we need more stack space in Debug because stack frames are larger
    set_target_properties(Luau.Analyze.CLI PROPERTIES LINK_FLAGS_DEBUG /STACK:2097152)
    set_target_properties(Luau.Repl.CLI PROPERTIES LINK_FLAGS_DEBUG /STACK:2097152)
    set_target_properties(Luau.UnitTest PROPERTIES LINK_FLAGS_DEBUG /STACK:2097152)
endif()

if(MSVC AND LUAU_BUILD_TESTS)
    # the default stack size that MSVC linker uses is 1 MB; we need more stack space in Debug because stack frames are larger
    set_target_properties(Luau.CLI.Test PROPERTIES LINK_FLAGS_DEBUG /STACK:2097152)
    set_target_properties(Luau.Conformance PROPERTIES LINK_FLAGS_DEBUG /STACK:2097152)
    set_target_properties(Luau.CLI.Test PROPERTIES LINK_FLAGS_OPTIMIZED /STACK:2097152)
    set_target_properties(Luau.Conformance PROPERTIES LINK_FLAGS_OPTIMIZED /STACK:2097152)
    set_target_properties(t_native_interop_test PROPERTIES LINK_FLAGS_DEBUG /STACK:2097152)
    set_target_properties(t_native_interop_test PROPERTIES LINK_FLAGS_OPTIMIZED /STACK:2097152)
endif()

# embed .natvis inside the library debug information
if(MSVC)
    target_link_options(Luau.Ast INTERFACE /NATVIS:${CMAKE_CURRENT_SOURCE_DIR}/tools/natvis/Ast.natvis)
    target_link_options(Luau.Analysis INTERFACE /NATVIS:${CMAKE_CURRENT_SOURCE_DIR}/tools/natvis/Analysis.natvis)
    target_link_options(Luau.CodeGen INTERFACE /NATVIS:${CMAKE_CURRENT_SOURCE_DIR}/tools/natvis/CodeGen.natvis)
    target_link_options(Luau.VM INTERFACE /NATVIS:${CMAKE_CURRENT_SOURCE_DIR}/tools/natvis/VM.natvis)
endif()

# make .natvis visible inside the solution
if(MSVC_IDE)
    target_sources(Luau.Ast PRIVATE tools/natvis/Ast.natvis)
    target_sources(Luau.Analysis PRIVATE tools/natvis/Analysis.natvis)
    target_sources(Luau.CodeGen PRIVATE tools/natvis/CodeGen.natvis)
    target_sources(Luau.VM PRIVATE tools/natvis/VM.natvis)
endif()

# On Windows and Android threads are provided, on Linux/Mac/iOS we use pthreads
add_library(osthreads INTERFACE)
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin|iOS")
    target_link_libraries(osthreads INTERFACE "-lpthread")
endif ()

if(LUAU_BUILD_CLI)
    target_compile_options(Luau.Repl.CLI PRIVATE ${LUAU_OPTIONS})
    target_compile_options(Luau.Reduce.CLI PRIVATE ${LUAU_OPTIONS})
    target_compile_options(Luau.Analyze.CLI PRIVATE ${LUAU_OPTIONS})
    target_compile_options(Luau.Ast.CLI PRIVATE ${LUAU_OPTIONS})
    target_compile_options(Luau.Compile.CLI PRIVATE ${LUAU_OPTIONS})
    target_compile_options(Luau.Bytecode.CLI PRIVATE ${LUAU_OPTIONS})

    if(LUAU_LINK_OPTIONS)
        target_link_options(Luau.Repl.CLI PRIVATE ${LUAU_LINK_OPTIONS})
        target_link_options(Luau.Reduce.CLI PRIVATE ${LUAU_LINK_OPTIONS})
        target_link_options(Luau.Analyze.CLI PRIVATE ${LUAU_LINK_OPTIONS})
        target_link_options(Luau.Ast.CLI PRIVATE ${LUAU_LINK_OPTIONS})
        target_link_options(Luau.Compile.CLI PRIVATE ${LUAU_LINK_OPTIONS})
        target_link_options(Luau.Bytecode.CLI PRIVATE ${LUAU_LINK_OPTIONS})
    endif()

    target_include_directories(Luau.Repl.CLI PRIVATE extern extern/isocline/include)

    # Static archives must be grouped so the linker can resolve symbols across them.
    # LspServer.cpp.o (in Luau.CLI.lib.a) references Analysis symbols that appear earlier
    # in the link list; --start-group forces multi-pass resolution on Linux/Android/BSD.
    if(CMAKE_SYSTEM_NAME MATCHES "Linux|Android|BSD|SunOS" AND NOT MSVC AND NOT APPLE)
        target_link_libraries(Luau.Repl.CLI PRIVATE -Wl,--start-group Luau.Compiler Luau.Analysis Luau.Inliner Luau.Config Luau.CodeGen Luau.VM Luau.Require Luau.CLI.lib isocline -Wl,--end-group)
        target_link_libraries(Luau.Analyze.CLI PRIVATE -Wl,--start-group Luau.Analysis Luau.CodeGen Luau.VM Luau.Require Luau.CLI.lib isocline -Wl,--end-group)
        target_link_libraries(Luau.Ast.CLI PRIVATE -Wl,--start-group Luau.Ast Luau.Analysis Luau.CLI.lib -Wl,--end-group)
    else()
        target_link_libraries(Luau.Repl.CLI PRIVATE Luau.Compiler Luau.Analysis Luau.Inliner Luau.Config Luau.CodeGen Luau.VM Luau.Require Luau.CLI.lib isocline)
        target_link_libraries(Luau.Analyze.CLI PRIVATE Luau.Analysis Luau.CLI.lib Luau.Require)
        target_link_libraries(Luau.Ast.CLI PRIVATE Luau.Ast Luau.Analysis Luau.CLI.lib)
    endif()

    target_link_libraries(Luau.Repl.CLI PRIVATE osthreads)
    target_link_libraries(Luau.Reduce.CLI PRIVATE osthreads)
    target_link_libraries(Luau.Analyze.CLI PRIVATE osthreads)
    target_link_libraries(Luau.Ast.CLI PRIVATE osthreads)
    target_link_libraries(Luau.Compile.CLI PRIVATE osthreads)
    target_link_libraries(Luau.Bytecode.CLI PRIVATE osthreads)

    target_compile_features(Luau.Reduce.CLI PRIVATE cxx_std_17)
    target_include_directories(Luau.Reduce.CLI PUBLIC Reduce/include)
    target_link_libraries(Luau.Reduce.CLI PRIVATE Luau.Common Luau.Ast Luau.Analysis Luau.CLI.lib)

    target_link_libraries(Luau.Compile.CLI PRIVATE Luau.Compiler Luau.VM Luau.CodeGen Luau.CLI.lib)

    target_link_libraries(Luau.Bytecode.CLI PRIVATE Luau.Compiler Luau.VM Luau.CodeGen Luau.CLI.lib)
endif()

if(LUAU_BUILD_TESTS)
    target_compile_options(Luau.UnitTest PRIVATE ${LUAU_OPTIONS})
    target_compile_definitions(Luau.UnitTest PRIVATE DOCTEST_CONFIG_DOUBLE_STRINGIFY DOCTEST_CONFIG_USE_STD_HEADERS)
    target_include_directories(Luau.UnitTest PRIVATE extern VM/include VM/src)
    target_link_libraries(Luau.UnitTest PRIVATE Luau.Analysis Luau.Bytecode Luau.Compiler Luau.CodeGen Luau.VM Luau.CLI.lib Luau.Require)

    target_compile_options(Luau.Conformance PRIVATE ${LUAU_OPTIONS})
    target_compile_definitions(Luau.Conformance PRIVATE DOCTEST_CONFIG_DOUBLE_STRINGIFY DOCTEST_CONFIG_USE_STD_HEADERS)
    target_include_directories(Luau.Conformance PRIVATE extern VM/src CLI/include)
    target_link_libraries(Luau.Conformance PRIVATE Luau.Analysis Luau.Bytecode Luau.Inliner Luau.Compiler Luau.CodeGen Luau.VM)
    if(CMAKE_SYSTEM_NAME MATCHES "Android|iOS")
        set(LUAU_CONFORMANCE_SOURCE_DIR "Client/Luau/tests/conformance")
    else ()
        file(REAL_PATH "tests/conformance" LUAU_CONFORMANCE_SOURCE_DIR)
    endif ()
    target_compile_definitions(Luau.Conformance PRIVATE LUAU_CONFORMANCE_SOURCE_DIR="${LUAU_CONFORMANCE_SOURCE_DIR}")

    target_compile_options(Luau.CLI.Test PRIVATE ${LUAU_OPTIONS})
    target_compile_definitions(Luau.CLI.Test PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS JACI_VERSION="${JACI_VERSION}")
    target_include_directories(Luau.CLI.Test PRIVATE extern CLI)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux|Android|BSD|SunOS" AND NOT MSVC AND NOT APPLE)
        target_link_libraries(Luau.CLI.Test PRIVATE -Wl,--start-group Luau.Compiler Luau.Analysis Luau.Inliner Luau.Config Luau.CodeGen Luau.VM Luau.Require Luau.CLI.lib isocline -Wl,--end-group)
    else()
        target_link_libraries(Luau.CLI.Test PRIVATE Luau.Compiler Luau.Analysis Luau.Inliner Luau.Config Luau.CodeGen Luau.VM Luau.Require Luau.CLI.lib isocline)
    endif()
    target_link_libraries(Luau.CLI.Test PRIVATE osthreads)

    target_sources(t_native_interop_test PRIVATE t/native_interop_test.cpp)
    target_compile_options(t_native_interop_test PRIVATE ${LUAU_OPTIONS})
    target_compile_features(t_native_interop_test PRIVATE cxx_std_17)
    target_include_directories(t_native_interop_test PRIVATE extern VM/src)
    target_link_libraries(t_native_interop_test PRIVATE Luau.Analysis Luau.Bytecode Luau.Compiler Luau.CodeGen Luau.VM Luau.Config)
    target_link_libraries(t_native_interop_test PRIVATE osthreads)

    if(LUAU_LINK_OPTIONS)
        target_link_options(Luau.UnitTest PRIVATE ${LUAU_LINK_OPTIONS})
        target_link_options(Luau.Conformance PRIVATE ${LUAU_LINK_OPTIONS})
        target_link_options(Luau.CLI.Test PRIVATE ${LUAU_LINK_OPTIONS})
        target_link_options(t_native_interop_test PRIVATE ${LUAU_LINK_OPTIONS})
    endif()

    add_subdirectory(fuzz)
endif()

if(LUAU_BUILD_WEB)
    target_compile_options(Luau.Web PRIVATE ${LUAU_OPTIONS})
    target_link_libraries(Luau.Web PRIVATE Luau.Compiler Luau.VM Luau.Analysis)

    # declare exported functions to emscripten
    target_link_options(Luau.Web PRIVATE -sEXPORTED_FUNCTIONS=["_executeScript","_checkScript"] -sEXPORTED_RUNTIME_METHODS=["ccall","cwrap"])

    # add -fexceptions for emscripten to allow exceptions to be caught in C++
    target_link_options(Luau.Web PRIVATE -fexceptions)

    # the output is a single .js file with an embedded wasm blob
    target_link_options(Luau.Web PRIVATE -sSINGLE_FILE=1)
endif()

# validate dependencies for internal libraries
foreach(LIB Luau.Ast Luau.Compiler Luau.Config Luau.Analysis Luau.CodeGen Luau.VM)
    if(TARGET ${LIB})
        get_target_property(DEPENDS ${LIB} LINK_LIBRARIES)
        # Match full Luau target names only: LLVM libraries (e.g. LLVMAnalysis)
        # must not trip the offline/runtime separation checks.
        if(LIB MATCHES "CodeGen|VM" AND DEPENDS MATCHES "Luau\\.(Ast|Analysis|Config|Compiler)")
            message(FATAL_ERROR ${LIB} " is a runtime component but it depends on one of the offline components")
        endif()
        if(LIB MATCHES "Ast|Compiler" AND DEPENDS MATCHES "Luau\\.(CodeGen|VM)")
            message(FATAL_ERROR ${LIB} " is an offline component but it depends on one of the runtime components")
        endif()
        if(LIB MATCHES "Ast|Compiler" AND DEPENDS MATCHES "Luau\\.(Analysis|Config)")
            message(FATAL_ERROR ${LIB} " is a compiler component but it depends on one of the analysis components")
        endif()
    endif()
endforeach()
