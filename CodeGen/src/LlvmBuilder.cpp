// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/LlvmBuilder.h"

#include <inttypes.h>

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

LlvmBuilder::LlvmBuilder(std::string moduleName)
    : moduleName(std::move(moduleName))
{
    irStream << "; ModuleID = '" << this->moduleName << "'\n";
    irStream << "source_filename = \"" << this->moduleName << ".jaci\"\n";
    irStream << "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128\"\n\n";

    // Common Jaci runtime declarations
    irStream << "declare ptr @luaH_new(ptr, i32, i32)\n";
    irStream << "declare void @luaC_barriertable(ptr, ptr, ptr)\n";
    irStream << "declare void @jaci_deoptimize_exit(ptr, i32, ptr)\n\n";
}

std::string LlvmBuilder::nextTemp(const std::string& prefix)
{
    return "%" + prefix + std::to_string(tempCounter++);
}

std::string LlvmBuilder::getLlvmTypeName(Type type) const
{
    switch (type.kind)
    {
    case TypeKind::Void:
        return "void";
    case TypeKind::Int1:
        return "i1";
    case TypeKind::Int8:
        return "i8";
    case TypeKind::Int32:
        return "i32";
    case TypeKind::Int64:
        return "i64";
    case TypeKind::Double:
        return "double";
    case TypeKind::Pointer:
        return "ptr";
    case TypeKind::TValue:
        return "{ double, i32, i32 }";
    case TypeKind::Vector:
        return "<2 x double>";
    }
    return "void";
}

std::string LlvmBuilder::getAliasScopeMetadata(AliasScopeKind scope) const
{
    switch (scope)
    {
    case AliasScopeKind::TableProperties:
        return ", !alias.scope !1";
    case AliasScopeKind::TableArray:
        return ", !alias.scope !2";
    case AliasScopeKind::UpvalueSlot:
        return ", !alias.scope !3";
    case AliasScopeKind::BufferBytes:
        return ", !alias.scope !4";
    default:
        return "";
    }
}

void LlvmBuilder::beginFunction(const std::string& name, Type returnType, const std::vector<Type>& paramTypes, const std::vector<std::string>& paramNames)
{
    currentFunctionName = name;
    tempCounter = 0;

    irStream << "define " << getLlvmTypeName(returnType) << " @" << name << "(";
    for (size_t i = 0; i < paramTypes.size(); ++i)
    {
        if (i > 0)
            irStream << ", ";
        irStream << getLlvmTypeName(paramTypes[i]) << " %" << (i < paramNames.size() ? paramNames[i] : ("arg" + std::to_string(i)));
    }
    irStream << ") {\n";
}

void LlvmBuilder::endFunction()
{
    irStream << "}\n\n";
    currentFunctionName.clear();
    currentBlockName.clear();
}

void LlvmBuilder::createBlock(const std::string& name)
{
    currentBlockName = name;
    irStream << name << ":\n";
}

void LlvmBuilder::setBlock(const std::string& name)
{
    currentBlockName = name;
}

std::string LlvmBuilder::getCurrentBlock() const
{
    return currentBlockName;
}

LlvmValue LlvmBuilder::constInt1(bool b)
{
    return LlvmValue(b ? "true" : "false", Type(TypeKind::Int1));
}

LlvmValue LlvmBuilder::constInt8(uint8_t val)
{
    return LlvmValue(std::to_string(val), Type(TypeKind::Int8));
}

LlvmValue LlvmBuilder::constInt32(int32_t val)
{
    return LlvmValue(std::to_string(val), Type(TypeKind::Int32));
}

LlvmValue LlvmBuilder::constInt64(int64_t val)
{
    return LlvmValue(std::to_string(val), Type(TypeKind::Int64));
}

LlvmValue LlvmBuilder::constDouble(double val)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", val);
    return LlvmValue(buf, Type(TypeKind::Double));
}

LlvmValue LlvmBuilder::constNullPointer()
{
    return LlvmValue("null", Type(TypeKind::Pointer));
}

LlvmValue LlvmBuilder::emitAdd(const LlvmValue& a, const LlvmValue& b)
{
    std::string res = nextTemp("add");
    if (a.type.isDouble())
    {
        irStream << "  " << res << " = fadd double " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, Type(TypeKind::Double));
    }
    else
    {
        irStream << "  " << res << " = add " << getLlvmTypeName(a.type) << " " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, a.type);
    }
}

LlvmValue LlvmBuilder::emitSub(const LlvmValue& a, const LlvmValue& b)
{
    std::string res = nextTemp("sub");
    if (a.type.isDouble())
    {
        irStream << "  " << res << " = fsub double " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, Type(TypeKind::Double));
    }
    else
    {
        irStream << "  " << res << " = sub " << getLlvmTypeName(a.type) << " " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, a.type);
    }
}

LlvmValue LlvmBuilder::emitMul(const LlvmValue& a, const LlvmValue& b)
{
    std::string res = nextTemp("mul");
    if (a.type.isDouble())
    {
        irStream << "  " << res << " = fmul double " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, Type(TypeKind::Double));
    }
    else
    {
        irStream << "  " << res << " = mul " << getLlvmTypeName(a.type) << " " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, a.type);
    }
}

LlvmValue LlvmBuilder::emitDiv(const LlvmValue& a, const LlvmValue& b)
{
    std::string res = nextTemp("div");
    if (a.type.isDouble())
    {
        irStream << "  " << res << " = fdiv double " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, Type(TypeKind::Double));
    }
    else
    {
        irStream << "  " << res << " = sdiv " << getLlvmTypeName(a.type) << " " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, a.type);
    }
}

LlvmValue LlvmBuilder::emitMod(const LlvmValue& a, const LlvmValue& b)
{
    std::string res = nextTemp("mod");
    if (a.type.isDouble())
    {
        irStream << "  " << res << " = frem double " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, Type(TypeKind::Double));
    }
    else
    {
        irStream << "  " << res << " = srem " << getLlvmTypeName(a.type) << " " << a.name << ", " << b.name << "\n";
        return LlvmValue(res, a.type);
    }
}

LlvmValue LlvmBuilder::emitICmp(const std::string& cond, const LlvmValue& a, const LlvmValue& b)
{
    std::string res = nextTemp("cmp");
    irStream << "  " << res << " = icmp " << cond << " " << getLlvmTypeName(a.type) << " " << a.name << ", " << b.name << "\n";
    return LlvmValue(res, Type(TypeKind::Int1));
}

LlvmValue LlvmBuilder::emitFCmp(const std::string& cond, const LlvmValue& a, const LlvmValue& b)
{
    std::string res = nextTemp("fcmp");
    irStream << "  " << res << " = fcmp " << cond << " double " << a.name << ", " << b.name << "\n";
    return LlvmValue(res, Type(TypeKind::Int1));
}

void LlvmBuilder::emitBranch(const std::string& targetBlock)
{
    irStream << "  br label %" << targetBlock << "\n";
}

void LlvmBuilder::emitCondBranch(const LlvmValue& cond, const std::string& thenBlock, const std::string& elseBlock)
{
    irStream << "  br i1 " << cond.name << ", label %" << thenBlock << ", label %" << elseBlock << "\n";
}

void LlvmBuilder::emitReturn(const LlvmValue& val)
{
    irStream << "  ret " << getLlvmTypeName(val.type) << " " << val.name << "\n";
}

void LlvmBuilder::emitReturnVoid()
{
    irStream << "  ret void\n";
}

LlvmValue LlvmBuilder::emitAlloca(Type type, const std::string& name)
{
    std::string res = name.empty() ? nextTemp("slot") : ("%" + name);
    irStream << "  " << res << " = alloca " << getLlvmTypeName(type) << ", align 8\n";
    return LlvmValue(res, Type(TypeKind::Pointer));
}

LlvmValue LlvmBuilder::emitLoad(Type type, const LlvmValue& ptr, AliasScopeKind aliasScope)
{
    std::string res = nextTemp("load");
    irStream << "  " << res << " = load " << getLlvmTypeName(type) << ", ptr " << ptr.name << getAliasScopeMetadata(aliasScope) << "\n";
    return LlvmValue(res, type);
}

void LlvmBuilder::emitStore(const LlvmValue& val, const LlvmValue& ptr, AliasScopeKind aliasScope)
{
    irStream << "  store " << getLlvmTypeName(val.type) << " " << val.name << ", ptr " << ptr.name << getAliasScopeMetadata(aliasScope) << "\n";
}

LlvmValue LlvmBuilder::emitGetElementPtr(Type elemType, const LlvmValue& ptr, const std::vector<LlvmValue>& indices)
{
    std::string res = nextTemp("gep");
    irStream << "  " << res << " = getelementptr inbounds " << getLlvmTypeName(elemType) << ", ptr " << ptr.name;
    for (const auto& idx : indices)
    {
        irStream << ", " << getLlvmTypeName(idx.type) << " " << idx.name;
    }
    irStream << "\n";
    return LlvmValue(res, Type(TypeKind::Pointer));
}

LlvmValue LlvmBuilder::emitShapeGuard(const LlvmValue& tablePtr, uint32_t expectedShapeId, const std::string& fallbackBlock)
{
    // Shape pointer at table offset 0 (i32)
    LlvmValue shapeLoad = emitLoad(Type(TypeKind::Int32), tablePtr, AliasScopeKind::TableHeader);
    LlvmValue expected = constInt32(int32_t(expectedShapeId));
    LlvmValue matches = emitICmp("eq", shapeLoad, expected);

    std::string continueBlock = nextTemp("shape_ok");
    emitCondBranch(matches, continueBlock, fallbackBlock);
    createBlock(continueBlock);
    return matches;
}

LlvmValue LlvmBuilder::emitLoadFieldSlot(const LlvmValue& tablePtr, uint32_t slotOffset, Type fieldType)
{
    LlvmValue slotIdx = constInt32(int32_t(slotOffset));
    LlvmValue fieldPtr = emitGetElementPtr(fieldType, tablePtr, {slotIdx});
    return emitLoad(fieldType, fieldPtr, AliasScopeKind::TableProperties);
}

void LlvmBuilder::emitStoreFieldSlot(const LlvmValue& tablePtr, uint32_t slotOffset, const LlvmValue& val)
{
    LlvmValue slotIdx = constInt32(int32_t(slotOffset));
    LlvmValue fieldPtr = emitGetElementPtr(val.type, tablePtr, {slotIdx});
    emitStore(val, fieldPtr, AliasScopeKind::TableProperties);
}

LlvmValue LlvmBuilder::emitLoadPackedArray(const LlvmValue& arrayPtr, const LlvmValue& index, ArraySpecialization spec)
{
    Type elemType = (spec == ArraySpecialization::PackedDouble) ? Type(TypeKind::Double) : Type(TypeKind::Int64);
    LlvmValue elemPtr = emitGetElementPtr(elemType, arrayPtr, {index});
    return emitLoad(elemType, elemPtr, AliasScopeKind::TableArray);
}

void LlvmBuilder::emitStorePackedArray(const LlvmValue& arrayPtr, const LlvmValue& index, const LlvmValue& val, ArraySpecialization spec)
{
    Type elemType = (spec == ArraySpecialization::PackedDouble) ? Type(TypeKind::Double) : Type(TypeKind::Int64);
    LlvmValue elemPtr = emitGetElementPtr(elemType, arrayPtr, {index});
    emitStore(val, elemPtr, AliasScopeKind::TableArray);
}

void LlvmBuilder::attachLoopVectorizeHint(bool enable)
{
    irStream << "  br label %loop_header, !llvm.loop !5\n";
}

LlvmValue LlvmBuilder::emitCall(const std::string& callee, Type retType, const std::vector<LlvmValue>& args)
{
    std::string res = retType.isVoid() ? "" : nextTemp("call");
    irStream << "  ";
    if (!res.empty())
        irStream << res << " = ";
    irStream << "call " << getLlvmTypeName(retType) << " @" << callee << "(";
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i > 0)
            irStream << ", ";
        irStream << getLlvmTypeName(args[i].type) << " " << args[i].name;
    }
    irStream << ")\n";
    return res.empty() ? LlvmValue() : LlvmValue(res, retType);
}

LlvmValue LlvmBuilder::emitFfiCall(const std::string& callee, Type retType, const std::vector<LlvmValue>& args, bool isPointer)
{
    std::string res = retType.isVoid() ? "" : nextTemp("ffi_ret");
    irStream << "  ";
    if (!res.empty())
        irStream << res << " = ";

    if (isPointer)
        irStream << "call ccc " << getLlvmTypeName(retType) << " " << callee << "(";
    else
        irStream << "call ccc " << getLlvmTypeName(retType) << " @" << callee << "(";

    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i > 0)
            irStream << ", ";
        irStream << getLlvmTypeName(args[i].type) << " " << args[i].name;
    }
    irStream << ")\n";
    return res.empty() ? LlvmValue() : LlvmValue(res, retType);
}

void LlvmBuilder::emitDeoptExit(uint32_t pcpos, const std::vector<std::pair<uint8_t, LlvmValue>>& liveRegs)
{
    LlvmValue pcVal = constInt32(int32_t(pcpos));
    emitCall("jaci_deoptimize_exit", Type(TypeKind::Void), {constNullPointer(), pcVal, constNullPointer()});
    emitReturnVoid();
}

std::string LlvmBuilder::getModuleIr() const
{
    std::string fullIr = irStream.str();
    fullIr += "\n!1 = !{!\"jaci.table.properties\"}\n";
    fullIr += "!2 = !{!\"jaci.table.array\"}\n";
    fullIr += "!3 = !{!\"jaci.upvalue\"}\n";
    fullIr += "!4 = !{!\"jaci.buffer\"}\n";
    fullIr += "!5 = distinct !{!5, !6}\n";
    fullIr += "!6 = !{!\"llvm.loop.vectorize.enable\", i1 true}\n";
    return fullIr;
}

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
