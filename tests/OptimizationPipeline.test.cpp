// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Hir.h"
#include "Luau/Mir.h"
#include "Luau/MirLowering.h"
#include "Luau/IrBuilder.h"
#include "Luau/IrDump.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("OptimizationPipeline");

TEST_CASE("EndToEndOptimizationPipeline")
{
    HostIrHooks hooks;
    IrBuilder ir(hooks);

    // 1. Construct input IR
    IrOp bMain = ir.block(IrBlockKind::Internal);
    ir.beginBlock(bMain);

    IrOp v1 = ir.constInt(15);
    IrOp v2 = ir.constInt(27);
    IrOp sum = ir.inst(IrCmd::ADD_INT, v1, v2);
    ir.inst(IrCmd::RETURN, sum);

    // 2. Lift existing IR -> HIR
    Hir::HirBuilder hirBuilder;
    Hir::Function hirFn = hirBuilder.liftFromIr(ir.function);

    CHECK_GE(hirFn.blocks.size(), 1);
    CHECK_GE(hirFn.instructions.size(), 1);

    // 3. Optimize HIR (semantic passes, constant propagation, table specialization, escape analysis)
    Hir::optimizeHir(hirFn);

    // 4. Lower HIR -> MIR (concrete representation types, explicit guards, memory operations)
    Mir::MirBuilder mirBuilder;
    Mir::Function mirFn = mirBuilder.lowerFromHir(hirFn);

    CHECK_GE(mirFn.blocks.size(), 1);

    // 5. Optimize MIR (redundant guard elimination, LICM, GVN/CSE, load/store elimination)
    Mir::optimizeMir(mirFn);

    // 6. Lower MIR -> Backend IR
    IrBuilder backendIr(hooks);
    bool lowerSuccess = Mir::lowerMirToIr(backendIr, mirFn, nullptr);

    CHECK(lowerSuccess);
    CHECK_GE(backendIr.function.blocks.size(), 1);
    CHECK_GE(backendIr.function.instructions.size(), 1);
}

TEST_CASE("EndToEndTableOptimizationPipeline")
{
    Hir::HirBuilder hbuilder;
    uint32_t b0 = hbuilder.createBlock("entry");
    hbuilder.setInsertionBlock(b0);

    Hir::Value tbl = hbuilder.emitAllocTable(0);
    Hir::Value k = hbuilder.emitConstString("count");
    Hir::Value v = hbuilder.emitConstInt(100);

    hbuilder.emitSetTable(tbl, k, v);
    Hir::Value getVal = hbuilder.emitGetTable(tbl, k);
    hbuilder.emitReturn({getVal});

    Hir::Function& hfn = hbuilder.getFunction();

    // Optimize HIR
    Hir::optimizeHir(hfn);

    // Lower to MIR
    Mir::MirBuilder mbuilder;
    Mir::Function mfn = mbuilder.lowerFromHir(hfn);

    // Optimize MIR
    Mir::optimizeMir(mfn);

    // Lower to Backend IR
    HostIrHooks hooks;
    IrBuilder backendIr(hooks);
    bool ok = Mir::lowerMirToIr(backendIr, mfn, nullptr);

    CHECK(ok);
}

TEST_SUITE_END();
