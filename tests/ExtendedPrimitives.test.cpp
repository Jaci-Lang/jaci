// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Type.h"
#include "Luau/TypeInfer.h"
#include "Luau/Compiler.h"

#include "Fixture.h"
#include "ScopedFlags.h"
#include "doctest.h"

using namespace Luau;

LUAU_FASTFLAG(LuauIntegerType2)

TEST_SUITE_BEGIN("ExtendedPrimitivesTest");

TEST_CASE_FIXTURE(BuiltinsFixture, "unit_and_void_types")
{
    CheckResult result = check(R"(
        local a: unit = nil
        local b: void = nil

        local function doNothing(): unit
            return nil
        end

        local function doNothingVoid(): void
            return nil
        end

        local r1: unit = doNothing()
        local r2: void = doNothingVoid()
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

TEST_CASE_FIXTURE(BuiltinsFixture, "sized_signed_integer_types_with_flag")
{
    ScopedFastFlag sff{FFlag::LuauIntegerType2, true};

    CheckResult result = check(R"(
        local a: int = 1i
        local b: int8 = 2i
        local c: int16 = 3i
        local d: int32 = 4i
        local e: int64 = 5i
        local f: i8 = 6i
        local g: i16 = 7i
        local h: i32 = 8i
        local i: i64 = 9i

        local function passSigned(x: int32, y: i64): int
            return x
        end

        local res = passSigned(d, i)
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

TEST_CASE_FIXTURE(BuiltinsFixture, "sized_unsigned_integer_types_with_flag")
{
    ScopedFastFlag sff{FFlag::LuauIntegerType2, true};

    CheckResult result = check(R"(
        local a: uint = 10i
        local b: uint8 = 20i
        local c: uint16 = 30i
        local d: uint32 = 40i
        local e: uint64 = 50i
        local f: u8 = 60i
        local g: u16 = 70i
        local h: u32 = 80i
        local i: u64 = 90i
        local j: byte = 100i

        local function passUnsigned(x: uint32, y: u8): uint
            return x
        end

        local res = passUnsigned(d, f)
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

TEST_CASE_FIXTURE(BuiltinsFixture, "floating_point_types")
{
    CheckResult result = check(R"(
        local a: float = 1.5
        local b: double = 2.5
        local c: float32 = 3.5
        local d: float64 = 4.5
        local e: f32 = 5.5
        local f: f64 = 6.5

        local function compute(x: float64, y: double): float
            return x * y
        end

        local res = compute(a, b)
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

TEST_CASE_FIXTURE(BuiltinsFixture, "integer_fallback_without_flag")
{
    ScopedFastFlag sff{FFlag::LuauIntegerType2, false};

    CheckResult result = check(R"(
        local a: int8 = 10
        local b: uint32 = 20
        local c: i64 = 30
        local d: u8 = 40
        local e: byte = 50
        local f: float32 = 60.5
        local g: unit = nil
        local h: void = nil
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

TEST_CASE("compiler_extended_primitive_types")
{
    std::string source = R"(
        local function process(u: unit, v: void, a: int8, b: uint16, c: i32, d: u64, e: float32, f: double)
            return a
        end
    )";

    CompileOptions options;
    std::string bytecode = compile(source, options);
    CHECK(!bytecode.empty());
}

TEST_SUITE_END();
