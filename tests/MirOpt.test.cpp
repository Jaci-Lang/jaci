// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Mir.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("MirOpt");

TEST_CASE("MirRedundantGuardElimination")
{
    Mir::MirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Mir::Value v = builder.emitConstInt32(10);
    Mir::Value g1 = builder.emitGuardTag(v, 3, 0);
    Mir::Value g2 = builder.emitGuardTag(v, 3, 0);

    Mir::Function& fn = builder.getFunction();
    Mir::optimizeMir(fn);

    CHECK_EQ(fn.instructions[g1.index].cmd, Mir::Cmd::GuardTag);
    CHECK_EQ(fn.instructions[g2.index].cmd, Mir::Cmd::Nop); // redundant guard eliminated
}

TEST_CASE("MirBoundsCheckElimination")
{
    Mir::MirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Mir::Value idx = builder.emitConstInt32(5);
    Mir::Value lim = builder.emitConstInt32(10);

    Mir::Value bce1 = builder.emitGuardBounds(idx, lim, 0);
    Mir::Value bce2 = builder.emitGuardBounds(idx, lim, 0);

    Mir::Function& fn = builder.getFunction();
    Mir::optimizeMir(fn);

    CHECK_EQ(fn.instructions[bce1.index].cmd, Mir::Cmd::GuardBounds);
    CHECK_EQ(fn.instructions[bce2.index].cmd, Mir::Cmd::Nop); // redundant bounds check eliminated
}

TEST_CASE("MirGvnCse")
{
    Mir::MirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Mir::Value a = builder.emitConstInt32(10);
    Mir::Value b = builder.emitConstInt32(20);

    Mir::Value add1 = builder.emitAddInt(a, b);
    Mir::Value add2 = builder.emitAddInt(a, b);
    builder.emitReturn({add1, add2});

    Mir::Function& fn = builder.getFunction();
    Mir::optimizeMir(fn);

    CHECK_EQ(fn.instructions[add1.index].cmd, Mir::Cmd::AddInt);
    CHECK_EQ(fn.instructions[add2.index].cmd, Mir::Cmd::Nop); // CSE replaced second addition
}

TEST_CASE("MirLicm")
{
    Mir::MirBuilder builder;
    uint32_t bPre = builder.createBlock("preheader");
    uint32_t bLoop = builder.createBlock("loop");
    uint32_t bExit = builder.createBlock("exit");

    builder.setInsertionBlock(bPre);
    Mir::Value a = builder.emitConstInt32(5);
    Mir::Value b = builder.emitConstInt32(10);
    builder.emitJump(bLoop);

    builder.setInsertionBlock(bLoop);
    Mir::Function& fn = builder.getFunction();
    fn.blocks[bLoop].isLoopHeader = true;

    Mir::Value invAdd = builder.emitAddInt(a, b);
    builder.emitBranchCond(builder.emitConstBool(true), bLoop, bExit);

    builder.setInsertionBlock(bExit);
    builder.emitReturn({});

    Mir::optimizeMir(fn);

    // Invariant addition should be hoisted to preheader
    bool foundInPreheader = false;
    for (uint32_t instIdx : fn.blocks[bPre].instIndices)
    {
        if (instIdx == invAdd.index)
            foundInPreheader = true;
    }
    CHECK(foundInPreheader);
}

TEST_CASE("MirLoadStoreElimination")
{
    Mir::MirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Mir::Value tbl = builder.emitAllocTable(0);
    Mir::Value v10 = builder.emitConstInt32(10);

    builder.emitStoreField(tbl, 0, v10);
    Mir::Value loadInst = builder.emitLoadField(tbl, 0, Mir::Type(Mir::TypeKind::Int32));

    Mir::Function& fn = builder.getFunction();
    Mir::optimizeMir(fn);

    // Load after store to same field forwards stored value
    CHECK_EQ(fn.instructions[loadInst.index].cmd, Mir::Cmd::Nop);
    REQUIRE_EQ(fn.instructions[loadInst.index].args.size(), 1);
    CHECK_EQ(fn.instructions[loadInst.index].args[0], v10);
}

TEST_CASE("MirGcBarrierElimination")
{
    Mir::MirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Mir::Value tbl = builder.emitAllocTable(0);
    Mir::Value child = builder.emitConstInt32(1);
    Mir::Value barrier = builder.emitGcWriteBarrier(tbl, child);

    Mir::Function& fn = builder.getFunction();
    Mir::optimizeMir(fn);

    // GC barrier on newly allocated table in same block is eliminated
    CHECK_EQ(fn.instructions[barrier.index].cmd, Mir::Cmd::Nop);
}

TEST_SUITE_END();
