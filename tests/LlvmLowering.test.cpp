// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Llvm.h"
#include "Luau/Mir.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("LlvmLowering");

TEST_CASE("LlvmModuleCreationAndTypes")
{
    Llvm::LlvmBuilder builder("test_module");

    std::vector<Llvm::Type> params = {Llvm::Type(Llvm::TypeKind::Int32), Llvm::Type(Llvm::TypeKind::Int32)};
    std::vector<std::string> paramNames = {"a", "b"};

    builder.beginFunction("add_numbers", Llvm::Type(Llvm::TypeKind::Int32), params, paramNames);
    builder.createBlock("entry");

    Llvm::LlvmValue a("%a", Llvm::Type(Llvm::TypeKind::Int32));
    Llvm::LlvmValue b("%b", Llvm::Type(Llvm::TypeKind::Int32));
    Llvm::LlvmValue sum = builder.emitAdd(a, b);

    builder.emitReturn(sum);
    builder.endFunction();

    std::string ir = builder.getModuleIr();
    CHECK(ir.find("define i32 @add_numbers(i32 %a, i32 %b)") != std::string::npos);
    CHECK(ir.find("add i32 %a, %b") != std::string::npos);
    CHECK(ir.find("ret i32") != std::string::npos);
}

TEST_CASE("LlvmLoweringFromMir")
{
    Mir::MirBuilder mbuilder;
    uint32_t bEntry = mbuilder.createBlock("entry");
    mbuilder.setInsertionBlock(bEntry);

    Mir::Value c10 = mbuilder.emitConstInt32(10);
    Mir::Value c20 = mbuilder.emitConstInt32(20);
    Mir::Value sum = mbuilder.emitAddInt(c10, c20);
    mbuilder.emitReturn({sum});

    Mir::Function& mfn = mbuilder.getFunction();

    Llvm::LlvmBuilder lbuilder("mir_to_llvm");
    Llvm::TableSpecializer tableSpec;

    bool ok = Llvm::lowerMirToLlvm(lbuilder, tableSpec, mfn, nullptr);
    CHECK(ok);

    std::string ir = lbuilder.getModuleIr();
    CHECK(ir.find("define i32 @jaci_entry") != std::string::npos);
    CHECK(ir.find("add i32") != std::string::npos);
    CHECK(ir.find("ret i32") != std::string::npos);
}

TEST_SUITE_END();
