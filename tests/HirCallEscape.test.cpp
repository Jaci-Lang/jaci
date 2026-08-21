// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Hir.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("HirCallEscape");

TEST_CASE("HirCallEffectClassification")
{
    Hir::HirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Hir::Value cfn = builder.emit(Hir::Cmd::AllocClosure);
    Hir::Value arg1 = builder.emitConstInt(10);
    Hir::Value callInst = builder.emit(Hir::Cmd::Call, Hir::Type(Hir::TypeKind::Any), {cfn, arg1});
    Hir::Value builtinInst = builder.emit(Hir::Cmd::CallBuiltin, Hir::Type(Hir::TypeKind::Number), {arg1});

    Hir::Function& fn = builder.getFunction();
    Hir::optimizeHir(fn);

    CHECK_EQ(fn.instructions[callInst.index].callEffect, Hir::CallEffect::Mutating);
    CHECK_EQ(fn.instructions[builtinInst.index].callEffect, Hir::CallEffect::Pure);
}

TEST_CASE("HirMultivalueAritySimplification")
{
    Hir::HirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Hir::Value v1 = builder.emitConstInt(1);
    Hir::Value v2 = builder.emitConstInt(2);
    Hir::Value v3 = builder.emitConstInt(3);

    Hir::Value pack = builder.emit(Hir::Cmd::PackMultivalue, Hir::Type(Hir::TypeKind::Any), {v1, v2, v3});

    Hir::Function& fn = builder.getFunction();
    Hir::optimizeHir(fn);

    CHECK_EQ(fn.instructions[pack.index].extra, 3);
}

TEST_CASE("HirSnapshotBuilding")
{
    Hir::HirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Hir::Value v = builder.emitConstInt(5);
    Hir::Value chk = builder.emit(Hir::Cmd::CheckTag, Hir::Type(Hir::TypeKind::Any), {v}, 3);

    Hir::Function& fn = builder.getFunction();
    Hir::optimizeHir(fn);

    CHECK_GE(fn.snapshots.size(), 1);
    CHECK_EQ(fn.instructions[chk.index].extra, 0); // attached snapshot 0
}

TEST_SUITE_END();
