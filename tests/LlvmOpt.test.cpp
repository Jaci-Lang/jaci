// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Llvm.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("LlvmOpt");

TEST_CASE("LlvmAliasScopesAndMemoryMetadata")
{
    Llvm::LlvmBuilder builder("alias_test");
    builder.beginFunction("prop_access", Llvm::Type(Llvm::TypeKind::Void), {Llvm::Type(Llvm::TypeKind::Pointer)}, {"tbl"});
    builder.createBlock("entry");

    Llvm::LlvmValue tbl("%tbl", Llvm::Type(Llvm::TypeKind::Pointer));
    Llvm::LlvmValue val = builder.emitLoadFieldSlot(tbl, 0, Llvm::Type(Llvm::TypeKind::Double));
    builder.emitStoreFieldSlot(tbl, 1, val);

    builder.emitReturnVoid();
    builder.endFunction();

    std::string ir = builder.getModuleIr();
    CHECK(ir.find("!alias.scope") != std::string::npos);
    CHECK(ir.find("jaci.table.properties") != std::string::npos);
}

// Requires a real LLVM engine; the no-LLVM build provides stubs only.
#if LUAU_USE_LLVM
TEST_CASE("LlvmEngineOptimization")
{
    Llvm::LlvmEngine engine;
    CHECK(engine.initialize());

    // Parse IR text, run the O3 pipeline, emit an object, place it in
    // executable memory, and run the resulting function.
    std::string sampleIr =
        "define i32 @foo() {\n"
        "entry:\n"
        "  %x = mul i32 6, 7\n"
        "  ret i32 %x\n"
        "}\n";

    void* modulePtr = engine.createModuleFromIrText(sampleIr);
    REQUIRE(modulePtr != nullptr);

    std::string object = engine.compileModuleToNativeObject(modulePtr, Llvm::OptLevel::O3);
    engine.releaseModule(modulePtr);

    REQUIRE_FALSE(object.empty());

    void* entry = engine.compileFunction(sampleIr, "foo", Llvm::OptLevel::O3);
    REQUIRE(entry != nullptr);

    int (*foo)() = reinterpret_cast<int (*)()>(entry);
    CHECK_EQ(foo(), 42);

    engine.releaseExecutable(entry);
}
#endif // LUAU_USE_LLVM

TEST_SUITE_END();
