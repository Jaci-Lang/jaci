// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/Llvm.h"
#include "doctest.h"

using namespace Luau::CodeGen;

TEST_SUITE_BEGIN("LlvmFfi");

TEST_CASE("LlvmFfiDirectCallEmission")
{
    Llvm::LlvmBuilder builder("ffi_module");
    builder.beginFunction("call_c_cos", Llvm::Type(Llvm::TypeKind::Double), {Llvm::Type(Llvm::TypeKind::Double)}, {"x"});
    builder.createBlock("entry");

    Llvm::LlvmValue argX("%x", Llvm::Type(Llvm::TypeKind::Double));
    Llvm::LlvmValue cosRet = builder.emitFfiCall("cos", Llvm::Type(Llvm::TypeKind::Double), {argX}, false);
    builder.emitReturn(cosRet);
    builder.endFunction();

    std::string ir = builder.getModuleIr();
    CHECK(ir.find("call ccc double @cos(double %x)") != std::string::npos);
    CHECK(ir.find("ret double") != std::string::npos);
}

TEST_CASE("LlvmFfiPointerDispatch")
{
    Llvm::LlvmBuilder builder("ffi_ptr_module");
    builder.beginFunction("invoke_fn_ptr", Llvm::Type(Llvm::TypeKind::Int32), {Llvm::Type(Llvm::TypeKind::Pointer), Llvm::Type(Llvm::TypeKind::Int32)}, {"fn_ptr", "val"});
    builder.createBlock("entry");

    Llvm::LlvmValue fnPtr("%fn_ptr", Llvm::Type(Llvm::TypeKind::Pointer));
    Llvm::LlvmValue val("%val", Llvm::Type(Llvm::TypeKind::Int32));

    Llvm::LlvmValue retVal = builder.emitFfiCall(fnPtr.name, Llvm::Type(Llvm::TypeKind::Int32), {val}, true);
    builder.emitReturn(retVal);
    builder.endFunction();

    std::string ir = builder.getModuleIr();
    CHECK(ir.find("call ccc i32 %fn_ptr(i32 %val)") != std::string::npos);
    CHECK(ir.find("ret i32") != std::string::npos);
}

TEST_SUITE_END();
