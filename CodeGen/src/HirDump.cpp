// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "Luau/HirDump.h"

#include <inttypes.h>
#include <stdio.h>

namespace Luau
{
namespace CodeGen
{
namespace Hir
{

const char* getCmdName(Cmd cmd)
{
    switch (cmd)
    {
    case Cmd::Nop:
        return "nop";
    case Cmd::ConstNil:
        return "const.nil";
    case Cmd::ConstBool:
        return "const.bool";
    case Cmd::ConstInt:
        return "const.int";
    case Cmd::ConstInt64:
        return "const.int64";
    case Cmd::ConstDouble:
        return "const.double";
    case Cmd::ConstString:
        return "const.string";
    case Cmd::ConstTag:
        return "const.tag";
    case Cmd::Phi:
        return "phi";
    case Cmd::BlockArg:
        return "blockarg";
    case Cmd::Jump:
        return "jump";
    case Cmd::Branch:
        return "branch";
    case Cmd::Return:
        return "return";
    case Cmd::Unreachable:
        return "unreachable";
    case Cmd::StructuredIf:
        return "struct.if";
    case Cmd::StructuredLoop:
        return "struct.loop";
    case Cmd::Add:
        return "add";
    case Cmd::Sub:
        return "sub";
    case Cmd::Mul:
        return "mul";
    case Cmd::Div:
        return "div";
    case Cmd::FloorDiv:
        return "floordiv";
    case Cmd::Mod:
        return "mod";
    case Cmd::Pow:
        return "pow";
    case Cmd::Neg:
        return "neg";
    case Cmd::Not:
        return "not";
    case Cmd::Len:
        return "len";
    case Cmd::BitAnd:
        return "bitand";
    case Cmd::BitOr:
        return "bitor";
    case Cmd::BitXor:
        return "bitxor";
    case Cmd::BitNot:
        return "bitnot";
    case Cmd::BitLShift:
        return "bitlshift";
    case Cmd::BitRShift:
        return "bitrshift";
    case Cmd::BitARShift:
        return "bitarshift";
    case Cmd::CmpEq:
        return "cmp.eq";
    case Cmd::CmpLt:
        return "cmp.lt";
    case Cmd::CmpLe:
        return "cmp.le";
    case Cmd::CheckTag:
        return "check.tag";
    case Cmd::CheckType:
        return "check.type";
    case Cmd::CheckNotNil:
        return "check.notnil";
    case Cmd::CheckShape:
        return "check.shape";
    case Cmd::CheckBounds:
        return "check.bounds";
    case Cmd::AssertTruth:
        return "assert.truth";
    case Cmd::Cast:
        return "cast";
    case Cmd::AllocTable:
        return "alloc.table";
    case Cmd::GetTable:
        return "get.table";
    case Cmd::SetTable:
        return "set.table";
    case Cmd::GetTableRaw:
        return "get.tableraw";
    case Cmd::SetTableRaw:
        return "set.tableraw";
    case Cmd::GetShapeSlot:
        return "get.shapeslot";
    case Cmd::SetShapeSlot:
        return "set.shapeslot";
    case Cmd::GetArrayElement:
        return "get.arrayelement";
    case Cmd::SetArrayElement:
        return "set.arrayelement";
    case Cmd::GetArrayLength:
        return "get.arraylength";
    case Cmd::TableLen:
        return "table.len";
    case Cmd::TableNext:
        return "table.next";
    case Cmd::AllocVirtualTable:
        return "alloc.virtualtable";
    case Cmd::ReadVirtualField:
        return "read.virtualfield";
    case Cmd::WriteVirtualField:
        return "write.virtualfield";
    case Cmd::MaterializeVirtual:
        return "materialize.virtual";
    case Cmd::AllocClosure:
        return "alloc.closure";
    case Cmd::GetUpvalue:
        return "get.upvalue";
    case Cmd::SetUpvalue:
        return "set.upvalue";
    case Cmd::CloseUpvalues:
        return "close.upvalues";
    case Cmd::Call:
        return "call";
    case Cmd::CallBuiltin:
        return "call.builtin";
    case Cmd::ReturnValues:
        return "return.values";
    case Cmd::PackMultivalue:
        return "pack.multivalue";
    case Cmd::ExtractMultivalue:
        return "extract.multivalue";
    case Cmd::GetVarargs:
        return "get.varargs";
    case Cmd::Snapshot:
        return "snapshot";
    case Cmd::GcBarrier:
        return "gc.barrier";
    case Cmd::LoadVmReg:
        return "load.vmreg";
    case Cmd::StoreVmReg:
        return "store.vmreg";
    case Cmd::VmExit:
        return "vmexit";
    }
    return "unknown";
}

const char* getTypeName(TypeKind kind)
{
    switch (kind)
    {
    case TypeKind::Bottom:
        return "bottom";
    case TypeKind::Nil:
        return "nil";
    case TypeKind::Boolean:
        return "boolean";
    case TypeKind::Number:
        return "number";
    case TypeKind::Integer:
        return "integer";
    case TypeKind::String:
        return "string";
    case TypeKind::Table:
        return "table";
    case TypeKind::Function:
        return "function";
    case TypeKind::Thread:
        return "thread";
    case TypeKind::Userdata:
        return "userdata";
    case TypeKind::Vector:
        return "vector";
    case TypeKind::Buffer:
        return "buffer";
    case TypeKind::Any:
        return "any";
    }
    return "any";
}

const char* getArrayStorageName(ArrayStorageKind kind)
{
    switch (kind)
    {
    case ArrayStorageKind::Generic:
        return "generic";
    case ArrayStorageKind::PackedInt:
        return "packed_int";
    case ArrayStorageKind::PackedDouble:
        return "packed_double";
    case ArrayStorageKind::PackedRef:
        return "packed_ref";
    case ArrayStorageKind::HoleyGeneric:
        return "holey_generic";
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
        return "%v" + std::to_string(value.index);
    case ValueKind::Constant:
    {
        if (value.index < function.constants.size())
        {
            const ConstantValue& cv = function.constants[value.index];
            switch (cv.kind)
            {
            case TypeKind::Nil:
                return "nil";
            case TypeKind::Boolean:
                return cv.val.b ? "true" : "false";
            case TypeKind::Integer:
                return std::to_string(cv.val.i);
            case TypeKind::Number:
                return std::to_string(cv.val.d);
            case TypeKind::String:
                return "\"" + cv.str + "\"";
            default:
                return "const#" + std::to_string(value.index);
            }
        }
        return "const#" + std::to_string(value.index);
    }
    case ValueKind::BlockArg:
        return "%arg" + std::to_string(value.index);
    case ValueKind::VirtualObject:
        return "%virt#" + std::to_string(value.index);
    case ValueKind::VmRegister:
        return "R" + std::to_string(value.index);
    case ValueKind::Multivalue:
        return "%mv#" + std::to_string(value.index);
    }
    return "unknown";
}

std::string toString(const Inst& inst, const Function& function)
{
    std::string out = getCmdName(inst.cmd);
    out += " [type: ";
    out += getTypeName(inst.type.kind);
    if (inst.type.optional)
        out += "?";
    out += "]";

    if (inst.range.isKnown())
    {
        out += " [range: " + std::to_string(inst.range.min) + ".." + std::to_string(inst.range.max) + "]";
    }

    if (!inst.stringData.empty())
    {
        out += " \"" + inst.stringData + "\"";
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
    std::string out = block.name + ":";
    if (!block.args.empty())
    {
        out += " (";
        for (size_t i = 0; i < block.args.size(); ++i)
        {
            if (i > 0)
                out += ", ";
            out += "%arg" + std::to_string(block.args[i].argIndex) + ": " + getTypeName(block.args[i].type.kind);
        }
        out += ")";
    }
    out += "\n";

    for (uint32_t instIdx : block.instIndices)
    {
        if (instIdx < function.instructions.size())
        {
            const Inst& inst = function.instructions[instIdx];
            out += "  %v" + std::to_string(instIdx) + " = " + toString(inst, function) + "\n";
        }
    }

    return out;
}

std::string toString(const Function& function)
{
    std::string out = "hir.function {\n";

    for (const TableShape& shape : function.tableShapes)
    {
        out += "  shape #" + std::to_string(shape.shapeId) + " [array: " + getArrayStorageName(shape.arrayLayout.kind) + "] {\n";
        for (const PropertySlot& slot : shape.slots)
        {
            out += "    slot " + std::to_string(slot.slotIndex) + ": " + slot.name + " (" + getTypeName(slot.expectedType.kind) + ")\n";
        }
        out += "  }\n";
    }

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

} // namespace Hir
} // namespace CodeGen
} // namespace Luau
