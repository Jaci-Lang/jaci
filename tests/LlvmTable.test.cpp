// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Llvm.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("LlvmTable");

TEST_CASE("LlvmTableShapeAndPIC")
{
    Llvm::TableSpecializer specializer;
    uint32_t shapeId = specializer.registerShape({"x", "y", "z"});

    CHECK(specializer.canBypassMetatable(shapeId, "x"));
    CHECK(specializer.canBypassMetatable(shapeId, "y"));
    CHECK_FALSE(specializer.canBypassMetatable(shapeId, "missing"));

    Llvm::PicSite& pic = specializer.getOrCreatePicSite(100, "x");
    pic.addEntry(shapeId, 0);

    CHECK_EQ(pic.findSlot(shapeId), 0);
    CHECK_EQ(pic.findSlot(999), -1);
}

TEST_CASE("LlvmPackedArrayAndVectorizationHints")
{
    Llvm::TableSpecializer specializer;
    uint32_t packedShape = specializer.registerShape({}, Llvm::ArraySpecialization::PackedDouble);

    const Llvm::TableLayoutDescriptor* layout = specializer.getLayout(packedShape);
    REQUIRE(layout);
    CHECK_EQ(layout->arraySpec, Llvm::ArraySpecialization::PackedDouble);
    CHECK_EQ(specializer.getArrayElementSize(Llvm::ArraySpecialization::PackedDouble), 8);
    CHECK_EQ(specializer.getArrayElementType(Llvm::ArraySpecialization::PackedDouble).kind, Llvm::TypeKind::Double);

    Llvm::LlvmBuilder builder("vec_test");
    builder.beginFunction("vec_loop", Llvm::Type(Llvm::TypeKind::Void), {Llvm::Type(Llvm::TypeKind::Pointer)}, {"arr"});
    builder.createBlock("entry");
    builder.createBlock("loop_header");

    Llvm::LlvmValue arr("%arr", Llvm::Type(Llvm::TypeKind::Pointer));
    Llvm::LlvmValue idx = builder.constInt32(0);
    Llvm::LlvmValue loaded = builder.emitLoadPackedArray(arr, idx, Llvm::ArraySpecialization::PackedDouble);
    builder.emitStorePackedArray(arr, idx, loaded, Llvm::ArraySpecialization::PackedDouble);

    builder.attachLoopVectorizeHint(true);
    builder.emitReturnVoid();
    builder.endFunction();

    std::string ir = builder.getModuleIr();
    CHECK(ir.find("llvm.loop.vectorize.enable") != std::string::npos);
    CHECK(ir.find("load double, ptr") != std::string::npos);
    CHECK(ir.find("store double") != std::string::npos);
    CHECK(ir.find("getelementptr inbounds double, ptr %arr") != std::string::npos);
}

TEST_CASE("LlvmStaticTableAssemblyDataPromotion")
{
    Llvm::TableSpecializer specializer;
    
    // Register static table array and dictionary without user explicit declaration
    uint32_t staticArrId = specializer.registerStaticArray({10.5, 20.25, 30.125, 40.0625});
    uint32_t staticDictId = specializer.registerStaticDictionary({{"width", 1920.0}, {"height", 1080.0}, {"fps", 60.0}});

    CHECK(specializer.isStaticTable(staticArrId));
    CHECK(specializer.isStaticTable(staticDictId));
    CHECK_FALSE(specializer.isStaticTable(9999));

    // Test static compile-time/AOT lookups
    auto elem1 = specializer.lookupStaticArrayElement(staticArrId, 1);
    CHECK(elem1.has_value());
    CHECK_EQ(*elem1, 20.25);

    auto widthVal = specializer.lookupStaticDouble(staticDictId, "width");
    CHECK(widthVal.has_value());
    CHECK_EQ(*widthVal, 1920.0);

    auto heightVal = specializer.lookupStaticDouble(staticDictId, "height");
    CHECK(heightVal.has_value());
    CHECK_EQ(*heightVal, 1080.0);

    // Verify static LLVM Assembly IR code generation
    Llvm::LlvmBuilder builder("static_data_test");
    builder.emitStaticDoubleArrayGlobal("@jaci_static_array_0", {10.5, 20.25, 30.125, 40.0625});
    builder.emitStaticStructGlobal("@jaci_static_dict_0", {1920.0, 1080.0, 60.0});

    builder.beginFunction("load_static_props", Llvm::Type(Llvm::TypeKind::Double), {}, {});
    builder.createBlock("entry");

    Llvm::LlvmValue idx = builder.constInt32(2);
    Llvm::LlvmValue arrVal = builder.emitLoadStaticDoubleArray("@jaci_static_array_0", 4, idx);
    Llvm::LlvmValue dictVal = builder.emitLoadStaticStructField("@jaci_static_dict_0", 0, Llvm::Type(Llvm::TypeKind::Double));

    Llvm::LlvmValue sum = builder.emitAdd(arrVal, dictVal);
    builder.emitReturn(sum);
    builder.endFunction();

    std::string ir = builder.getModuleIr();
    CHECK(ir.find("@jaci_static_array_0 = internal constant [4 x double]") != std::string::npos);
    CHECK(ir.find("@jaci_static_dict_0 = internal constant { double, double, double }") != std::string::npos);
    CHECK(ir.find("getelementptr inbounds [4 x double], ptr @jaci_static_array_0") != std::string::npos);
    CHECK(ir.find("getelementptr inbounds ptr, ptr @jaci_static_dict_0") != std::string::npos);
}

TEST_CASE("LlvmTableFreezeOptimization")
{
    Llvm::LlvmBuilder builder("table_freeze_test");
    builder.beginFunction("freeze_table_fn", Llvm::Type(Llvm::TypeKind::Pointer), {Llvm::Type(Llvm::TypeKind::Pointer)}, {"tbl"});
    builder.createBlock("entry");

    Llvm::LlvmValue tbl("%tbl", Llvm::Type(Llvm::TypeKind::Pointer));
    Llvm::LlvmValue frozen = builder.emitTableFreeze(tbl);
    builder.emitReturn(frozen);
    builder.endFunction();

    std::string ir = builder.getModuleIr();
    CHECK(ir.find("store i8 1, ptr") != std::string::npos);
}

TEST_SUITE_END();
