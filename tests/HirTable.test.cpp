// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Hir.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("HirTable");

TEST_CASE("HirTableShapeSpecialization")
{
    Hir::HirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Hir::Value tbl = builder.emitAllocTable(0);
    Hir::Value keyX = builder.emitConstString("x");
    Hir::Value val10 = builder.emitConstInt(10);

    Hir::Value setInst = builder.emitSetTable(tbl, keyX, val10);
    Hir::Value getInst = builder.emitGetTable(tbl, keyX);
    // Table escapes via call, preserving concrete shape slots instead of virtual scalar replacement
    builder.emit(Hir::Cmd::Call, Hir::Type(Hir::TypeKind::Any), {tbl});

    Hir::Function& fn = builder.getFunction();
    Hir::optimizeHir(fn);

    // GetTable and SetTable with constant string key should specialize to shape slot accesses
    CHECK_EQ(fn.instructions[setInst.index].cmd, Hir::Cmd::SetShapeSlot);
    CHECK_EQ(fn.instructions[getInst.index].cmd, Hir::Cmd::GetShapeSlot);
    CHECK_EQ(fn.instructions[setInst.index].extra, fn.instructions[getInst.index].extra);
    CHECK_GE(fn.tableShapes.size(), 1);
    CHECK_EQ(fn.tableShapes[0].slots[0].name, "x");
}

TEST_CASE("HirTypedArrayLayout")
{
    Hir::HirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Hir::Value tbl = builder.emitAllocTable(0);
    Hir::Value idx1 = builder.emitConstInt(1);
    Hir::Value val99 = builder.emitConstInt(99);

    Hir::Value setArr = builder.emitSetTable(tbl, idx1, val99);
    Hir::Value getArr = builder.emitGetTable(tbl, idx1);
    builder.emit(Hir::Cmd::Call, Hir::Type(Hir::TypeKind::Any), {tbl});

    Hir::Function& fn = builder.getFunction();
    Hir::optimizeHir(fn);

    CHECK_EQ(fn.instructions[setArr.index].cmd, Hir::Cmd::SetArrayElement);
    CHECK_EQ(fn.instructions[getArr.index].cmd, Hir::Cmd::GetArrayElement);
}

TEST_CASE("HirVirtualTableScalarReplacement")
{
    Hir::HirBuilder builder;
    uint32_t bEntry = builder.createBlock("entry");
    builder.setInsertionBlock(bEntry);

    Hir::Value tbl = builder.emitAllocTable(0);
    Hir::Value keyY = builder.emitConstString("y");
    Hir::Value val42 = builder.emitConstInt(42);

    builder.emitSetTable(tbl, keyY, val42);
    Hir::Value getInst = builder.emitGetTable(tbl, keyY);
    builder.emitReturn({getInst});

    Hir::Function& fn = builder.getFunction();
    Hir::optimizeHir(fn);

    // Since tbl does not escape, it becomes a VirtualTable and field access is replaced
    CHECK_GE(fn.virtualTables.size(), 1);
    CHECK_EQ(fn.virtualTables[0].escape, Hir::EscapeState::NoEscape);
    CHECK_EQ(fn.instructions[getInst.index].cmd, Hir::Cmd::ReadVirtualField);
}

TEST_SUITE_END();
