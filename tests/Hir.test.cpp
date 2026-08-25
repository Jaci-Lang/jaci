// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Hir.h"
#include "Luau/IrBuilder.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("Hir");

TEST_CASE("HirDataStructuresAndSSA")
{
    Hir::Function function;
    uint32_t b0 = function.createBlock("entry");
    uint32_t b1 = function.createBlock("exit");

    CHECK_EQ(b0, 0);
    CHECK_EQ(b1, 1);
    CHECK_EQ(function.blocks.size(), 2);
    CHECK_EQ(function.blocks[0].name, "entry");
    CHECK_EQ(function.blocks[1].name, "exit");

    Hir::HirBuilder builder;
    uint32_t entry = builder.createBlock("entry");
    builder.setInsertionBlock(entry);

    Hir::Value c1 = builder.emitConstInt(10);
    Hir::Value c2 = builder.emitConstInt(20);
    Hir::Value sum = builder.emitAdd(c1, c2, Hir::Type(Hir::TypeKind::Integer));

    CHECK_EQ(c1.kind, Hir::ValueKind::Inst);
    CHECK_EQ(c2.kind, Hir::ValueKind::Inst);
    CHECK_EQ(sum.kind, Hir::ValueKind::Inst);

    Hir::Function& fn = builder.getFunction();
    CHECK_EQ(fn.instructions.size(), 3);
    CHECK_EQ(fn.instructions[sum.index].cmd, Hir::Cmd::Add);
}

TEST_CASE("HirConstantFoldingAndRangePropagation")
{
    Hir::HirBuilder builder;
    uint32_t entry = builder.createBlock("entry");
    builder.setInsertionBlock(entry);

    Hir::Value c10 = builder.emitConstInt(10);
    Hir::Value c25 = builder.emitConstInt(25);
    Hir::Value addInst = builder.emitAdd(c10, c25, Hir::Type(Hir::TypeKind::Integer));

    Hir::Function& fn = builder.getFunction();
    Hir::optimizeHir(fn);

    // Instruction should have folded to ConstInt(35)
    CHECK_EQ(fn.instructions[addInst.index].cmd, Hir::Cmd::ConstInt);
    CHECK_EQ(fn.instructions[addInst.index].range.min, 35);
    CHECK_EQ(fn.instructions[addInst.index].range.max, 35);
}

TEST_CASE("HirControlFlowAndAnalysis")
{
    Hir::HirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    uint32_t bThen = builder.createBlock("then_block");
    uint32_t bElse = builder.createBlock("else_block");
    uint32_t bMerge = builder.createBlock("merge_block");

    builder.setInsertionBlock(bEntry);
    Hir::Value cond = builder.emitConstBool(true);
    builder.emitBranch(cond, bThen, bElse);

    builder.setInsertionBlock(bThen);
    builder.emitJump(bMerge);

    builder.setInsertionBlock(bElse);
    builder.emitJump(bMerge);

    builder.setInsertionBlock(bMerge);
    builder.emitReturn({});

    Hir::Function& fn = builder.getFunction();
    Hir::HirAnalysis analysis(fn);
    analysis.runPasses();

    CHECK(analysis.isDominating(bEntry, bThen));
    CHECK(analysis.isDominating(bEntry, bElse));
    CHECK(analysis.isDominating(bEntry, bMerge));
    CHECK_FALSE(analysis.isDominating(bThen, bMerge));
}

TEST_CASE("HirLoopAnalysisAndWidening")
{
    Hir::HirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    uint32_t bHeader = builder.createBlock("loop_header");
    uint32_t bBody = builder.createBlock("loop_body");
    uint32_t bExit = builder.createBlock("loop_exit");

    builder.setInsertionBlock(bEntry);
    builder.emitJump(bHeader);

    builder.setInsertionBlock(bHeader);
    Hir::Function& fn = builder.getFunction();
    fn.blocks[bHeader].isLoopHeader = true;

    Hir::StructuredLoop loop;
    loop.kind = Hir::LoopKind::Numeric;
    loop.inductionRange = Hir::Range(1, 100);
    loop.headerBlock = bHeader;
    loop.bodyBlock = bBody;
    loop.exitBlock = bExit;
    fn.blocks[bHeader].structuredLoop = loop;

    builder.emitBranch(builder.emitConstBool(true), bBody, bExit);

    builder.setInsertionBlock(bBody);
    builder.emitJump(bHeader);

    builder.setInsertionBlock(bExit);
    builder.emitReturn({});

    Hir::HirAnalysis analysis(fn);
    analysis.runPasses();

    const Hir::LoopAnalysisResult* linfo = analysis.getLoopInfo(bHeader);
    REQUIRE(linfo != nullptr);
    CHECK(linfo->isAnalyzed);
    CHECK(linfo->isInductionKnown);
    CHECK_EQ(linfo->inductionRange.min, 1);
    CHECK_EQ(linfo->inductionRange.max, 100);
}

TEST_SUITE_END();
