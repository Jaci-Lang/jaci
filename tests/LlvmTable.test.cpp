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

TEST_SUITE_END();
