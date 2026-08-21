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

TEST_CASE("LlvmEngineOptimization")
{
    Llvm::LlvmEngine engine;
    CHECK(engine.initialize());

    std::string sampleIr = "define i32 @foo() { ret i32 42 }\n";
    CHECK(engine.optimizeModule(sampleIr, Llvm::OptLevel::O3));
}

TEST_SUITE_END();
