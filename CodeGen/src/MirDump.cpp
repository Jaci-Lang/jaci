// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/MirDump.h"

#include <stdio.h>

namespace Luau
{
namespace CodeGen
{
namespace Mir
{

const char* getCmdName(Cmd cmd)
{
    switch (cmd)
    {
    case Cmd::Nop:
        return "nop";
    case Cmd::ConstBool:
        return "const.bool";
    case Cmd::ConstInt32:
        return "const.i32";
    case Cmd::ConstInt64:
        return "const.i64";
    case Cmd::ConstFloat64:
        return "const.f64";
    case Cmd::ConstTag:
        return "const.tag";
    case Cmd::ConstNull:
        return "const.null";
    case Cmd::BlockArg:
        return "blockarg";
    case Cmd::Jump:
        return "jump";
    case Cmd::BranchCond:
        return "branch.cond";
    case Cmd::Switch:
        return "switch";
    case Cmd::Return:
        return "return";
    case Cmd::Unreachable:
        return "unreachable";
    case Cmd::VmExit:
        return "vmexit";
    case Cmd::AddInt:
        return "add.int";
    case Cmd::SubInt:
        return "sub.int";
    case Cmd::MulInt:
        return "mul.int";
    case Cmd::DivInt:
        return "div.int";
    case Cmd::ModInt:
        return "mod.int";
    case Cmd::AndInt:
        return "and.int";
    case Cmd::OrInt:
        return "or.int";
    case Cmd::XorInt:
        return "xor.int";
    case Cmd::ShlInt:
        return "shl.int";
    case Cmd::ShrInt:
        return "shr.int";
    case Cmd::SarInt:
        return "sar.int";
    case Cmd::NegInt:
        return "neg.int";
    case Cmd::NotInt:
        return "not.int";
    case Cmd::AddFloat:
        return "add.float";
    case Cmd::SubFloat:
        return "sub.float";
    case Cmd::MulFloat:
        return "mul.float";
    case Cmd::DivFloat:
        return "div.float";
    case Cmd::NegFloat:
        return "neg.float";
    case Cmd::AbsFloat:
        return "abs.float";
    case Cmd::SqrtFloat:
        return "sqrt.float";
    case Cmd::CmpEqInt:
        return "cmp.eq.int";
    case Cmd::CmpLtInt:
        return "cmp.lt.int";
    case Cmd::CmpLeInt:
        return "cmp.le.int";
    case Cmd::CmpEqFloat:
        return "cmp.eq.float";
    case Cmd::CmpLtFloat:
        return "cmp.lt.float";
    case Cmd::CmpLeFloat:
        return "cmp.le.float";
    case Cmd::CmpEqPtr:
        return "cmp.eq.ptr";
    case Cmd::IntToFloat:
        return "conv.i2f";
    case Cmd::FloatToInt:
        return "conv.f2i";
    case Cmd::BoxValue:
        return "box";
    case Cmd::UnboxValue:
        return "unbox";
    case Cmd::GetTag:
        return "get.tag";
    case Cmd::GetPayload:
        return "get.payload";
    case Cmd::GuardTag:
        return "guard.tag";
    case Cmd::GuardShape:
        return "guard.shape";
    case Cmd::GuardBounds:
        return "guard.bounds";
    case Cmd::GuardNotNil:
        return "guard.notnil";
    case Cmd::GuardCondition:
        return "guard.cond";
    case Cmd::Load:
        return "load";
    case Cmd::Store:
        return "store";
    case Cmd::LoadField:
        return "load.field";
    case Cmd::StoreField:
        return "store.field";
    case Cmd::LoadArrayElement:
        return "load.arrelem";
    case Cmd::StoreArrayElement:
        return "store.arrelem";
    case Cmd::GetElementPtr:
        return "getelementptr";
    case Cmd::AllocTable:
        return "alloc.table";
    case Cmd::AllocClosure:
        return "alloc.closure";
    case Cmd::AllocBuffer:
        return "alloc.buffer";
    case Cmd::GcWriteBarrier:
        return "gc.barrier";
    case Cmd::CallDirect:
        return "call.direct";
    case Cmd::CallIndirect:
        return "call.indirect";
    case Cmd::CallBuiltin:
        return "call.builtin";
    case Cmd::Snapshot:
        return "snapshot";
    }
    return "unknown";
}

const char* getTypeName(TypeKind kind)
{
    switch (kind)
    {
    case TypeKind::Void:
        return "void";
    case TypeKind::Bool:
        return "bool";
    case TypeKind::Int32:
        return "i32";
    case TypeKind::Int64:
        return "i64";
    case TypeKind::Float64:
        return "f64";
    case TypeKind::Tag:
        return "tag";
    case TypeKind::RawPtr:
        return "ptr";
    case TypeKind::GcPtr:
        return "gcptr";
    case TypeKind::ManagedRef:
        return "managedref";
    case TypeKind::TValueBoxed:
        return "tvalue";
    }
    return "unknown";
}

const char* getLocationClassName(LocationClass lc)
{
    switch (lc)
    {
    case LocationClass::GenericHeap:
        return "heap";
    case LocationClass::TableHeader:
        return "table.header";
    case LocationClass::TableProperties:
        return "table.props";
    case LocationClass::TableArray:
        return "table.array";
    case LocationClass::UpvalueSlot:
        return "upval";
    case LocationClass::BufferBytes:
        return "buffer";
    case LocationClass::GlobalEnv:
        return "global";
    case LocationClass::VmRegisterStack:
        return "stack";
    }
    return "unknown";
}

std::string toString(const Value& value, const Function& function)
{
    switch (value.kind)
    {
    case ValueKind::None:
        return "none";
    case ValueKind::Inst:
        return "%m" + std::to_string(value.index);
    case ValueKind::Constant:
    {
        if (value.index < function.constants.size())
        {
            const ConstantValue& cv = function.constants[value.index];
            switch (cv.kind)
            {
            case TypeKind::Bool:
                return cv.val.b ? "true" : "false";
            case TypeKind::Int32:
                return std::to_string(cv.val.i32);
            case TypeKind::Int64:
                return std::to_string(cv.val.i64);
            case TypeKind::Float64:
                return std::to_string(cv.val.f64);
            case TypeKind::Tag:
                return "tag#" + std::to_string(cv.val.tag);
            default:
                return "const#" + std::to_string(value.index);
            }
        }
        return "const#" + std::to_string(value.index);
    }
    case ValueKind::BlockArg:
        return "%m_arg" + std::to_string(value.index);
    }
    return "unknown";
}

std::string toString(const Inst& inst, const Function& function)
{
    std::string out = getCmdName(inst.cmd);
    out += " [";
    out += getTypeName(inst.type.kind);
    out += "]";

    if (inst.memoryEffect != MemoryEffect::None)
    {
        out += " <" + std::string(getLocationClassName(inst.locationClass)) + ">";
    }

    if (inst.offset != 0)
    {
        out += " [offset: " + std::to_string(inst.offset) + "]";
    }

    if (!inst.args.empty())
    {
        out += " (";
        for (size_t i = 0; i < inst.args.size(); ++i)
        {
            if (i > 0)
                out += ", ";
            out += toString(inst.args[i], function);
        }
        out += ")";
    }

    return out;
}

std::string toString(const Block& block, const Function& function)
{
    std::string out = block.name + ":\n";
    for (uint32_t instIdx : block.instIndices)
    {
        if (instIdx < function.instructions.size())
        {
            const Inst& inst = function.instructions[instIdx];
            out += "  %m" + std::to_string(instIdx) + " = " + toString(inst, function) + "\n";
        }
    }
    return out;
}

std::string toString(const Function& function)
{
    std::string out = "mir.function {\n";
    for (const Block& block : function.blocks)
    {
        out += toString(block, function);
    }
    out += "}\n";
    return out;
}

void dump(const Function& function)
{
    std::string s = toString(function);
    printf("%s\n", s.c_str());
}

} // namespace Mir
} // namespace CodeGen
} // namespace Luau
