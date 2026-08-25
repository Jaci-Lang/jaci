// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Hir.h"
#include "Luau/Mir.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("Mir");

TEST_CASE("MirConstructionAndLoweringFromHir")
{
    Hir::HirBuilder hbuilder;
    uint32_t hbEntry = hbuilder.createBlock("entry");
    hbuilder.setInsertionBlock(hbEntry);

    Hir::Value c10 = hbuilder.emitConstInt(10);
    Hir::Value c20 = hbuilder.emitConstInt(20);
    Hir::Value sum = hbuilder.emitAdd(c10, c20, Hir::Type(Hir::TypeKind::Integer));
    hbuilder.emitReturn({sum});

    Hir::Function& hfn = hbuilder.getFunction();

    Mir::MirBuilder mbuilder;
    Mir::Function mfn = mbuilder.lowerFromHir(hfn);

    CHECK_EQ(mfn.blocks.size(), 1);
    CHECK_GE(mfn.instructions.size(), 2);
}

TEST_CASE("MirRepresentationTypesAndGuards")
{
    Mir::MirBuilder mbuilder;
    uint32_t bEntry = mbuilder.createBlock("entry");
    mbuilder.setInsertionBlock(bEntry);

    Mir::Value vInt = mbuilder.emitConstInt32(42);
    Mir::Value vFlt = mbuilder.emitConstFloat64(3.14);

    Mir::Value gTag = mbuilder.emitGuardTag(vInt, 3, 0);
    Mir::Value gBounds = mbuilder.emitGuardBounds(vInt, vInt, 0);

    Mir::Function& fn = mbuilder.getFunction();

    CHECK_EQ(fn.instructions[vFlt.index].cmd, Mir::Cmd::ConstFloat64);
    CHECK_EQ(fn.instructions[gTag.index].cmd, Mir::Cmd::GuardTag);
    CHECK_EQ(fn.instructions[gBounds.index].cmd, Mir::Cmd::GuardBounds);
}

TEST_CASE("MirTableMemoryModeling")
{
    Mir::MirBuilder mbuilder;
    uint32_t bEntry = mbuilder.createBlock("entry");
    mbuilder.setInsertionBlock(bEntry);

    Mir::Value tbl = mbuilder.emitAllocTable(0);
    Mir::Value val = mbuilder.emitConstInt32(100);

    Mir::Value storeInst = mbuilder.emitStoreField(tbl, 0, val);
    Mir::Value loadInst = mbuilder.emitLoadField(tbl, 0, Mir::Type(Mir::TypeKind::Int32));

    Mir::Function& fn = mbuilder.getFunction();

    CHECK_EQ(fn.instructions[storeInst.index].locationClass, Mir::LocationClass::TableProperties);
    CHECK_EQ(fn.instructions[storeInst.index].memoryEffect, Mir::MemoryEffect::Write);
    CHECK_EQ(fn.instructions[loadInst.index].locationClass, Mir::LocationClass::TableProperties);
    CHECK_EQ(fn.instructions[loadInst.index].memoryEffect, Mir::MemoryEffect::Read);
}

TEST_SUITE_END();
