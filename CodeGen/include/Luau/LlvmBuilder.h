// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#pragma once

#include "Luau/LlvmData.h"

#include <string>
#include <vector>
#include <sstream>

namespace Luau
{
namespace CodeGen
{
namespace Llvm
{

struct LlvmValue
{
    std::string name;
    Type type;

    LlvmValue() = default;
    LlvmValue(std::string name, Type type)
        : name(std::move(name))
        , type(type)
    {
    }

    bool empty() const
    {
        return name.empty();
    }
};

class LlvmBuilder
{
public:
    explicit LlvmBuilder(std::string moduleName = "jaci_module");

    void beginFunction(const std::string& name, Type returnType, const std::vector<Type>& paramTypes, const std::vector<std::string>& paramNames);
    void endFunction();

    void createBlock(const std::string& name);
    void setBlock(const std::string& name);
    std::string getCurrentBlock() const;

    // Constants & Values
    LlvmValue constInt1(bool b);
    LlvmValue constInt8(uint8_t val);
    LlvmValue constInt32(int32_t val);
    LlvmValue constInt64(int64_t val);
    LlvmValue constDouble(double val);
    LlvmValue constNullPointer();

    // Arithmetic & Logic
    LlvmValue emitAdd(const LlvmValue& a, const LlvmValue& b);
    LlvmValue emitSub(const LlvmValue& a, const LlvmValue& b);
    LlvmValue emitMul(const LlvmValue& a, const LlvmValue& b);
    LlvmValue emitDiv(const LlvmValue& a, const LlvmValue& b);
    LlvmValue emitMod(const LlvmValue& a, const LlvmValue& b);

    // Comparisons
    LlvmValue emitICmp(const std::string& cond, const LlvmValue& a, const LlvmValue& b);
    LlvmValue emitFCmp(const std::string& cond, const LlvmValue& a, const LlvmValue& b);

    // Control Flow
    void emitBranch(const std::string& targetBlock);
    void emitCondBranch(const LlvmValue& cond, const std::string& thenBlock, const std::string& elseBlock);
    void emitReturn(const LlvmValue& val);
    void emitReturnVoid();

    // Memory Operations & Alias Scopes
    LlvmValue emitAlloca(Type type, const std::string& name = "");
    LlvmValue emitLoad(Type type, const LlvmValue& ptr, AliasScopeKind aliasScope = AliasScopeKind::GeneralHeap);
    void emitStore(const LlvmValue& val, const LlvmValue& ptr, AliasScopeKind aliasScope = AliasScopeKind::GeneralHeap);
    LlvmValue emitGetElementPtr(Type elemType, const LlvmValue& ptr, const std::vector<LlvmValue>& indices);

    // Table and Array Operations
    LlvmValue emitShapeGuard(const LlvmValue& tablePtr, uint32_t expectedShapeId, const std::string& fallbackBlock);
    LlvmValue emitLoadFieldSlot(const LlvmValue& tablePtr, uint32_t slotOffset, Type fieldType);
    void emitStoreFieldSlot(const LlvmValue& tablePtr, uint32_t slotOffset, const LlvmValue& val);

    LlvmValue emitLoadPackedArray(const LlvmValue& arrayPtr, const LlvmValue& index, ArraySpecialization spec);
    void emitStorePackedArray(const LlvmValue& arrayPtr, const LlvmValue& index, const LlvmValue& val, ArraySpecialization spec);

    // Vectorization & Loop Hints
    void attachLoopVectorizeHint(bool enable = true);

    // Static Table & Assembly Data Emission (Zero runtime allocation & static constant loads)
    void emitStaticDoubleArrayGlobal(const std::string& globalSymbol, const std::vector<double>& values);
    void emitStaticInt64ArrayGlobal(const std::string& globalSymbol, const std::vector<int64_t>& values);
    void emitStaticStructGlobal(const std::string& globalSymbol, const std::vector<double>& fieldValues);
    LlvmValue emitLoadStaticDoubleArray(const std::string& globalSymbol, size_t size, const LlvmValue& index);
    LlvmValue emitLoadStaticStructField(const std::string& globalSymbol, uint32_t fieldIndex, Type fieldType);
    LlvmValue emitTableFreeze(const LlvmValue& tablePtr);

    // External Calls & Deoptimization
    LlvmValue emitCall(const std::string& callee, Type retType, const std::vector<LlvmValue>& args);
    LlvmValue emitFfiCall(const std::string& callee, Type retType, const std::vector<LlvmValue>& args, bool isPointer = false);
    void emitDeoptExit(uint32_t pcpos, const std::vector<std::pair<uint8_t, LlvmValue>>& liveRegs);

    // Complete Module IR string
    std::string getModuleIr() const;

private:
    std::string moduleName;
    std::stringstream irStream;
    std::string currentFunctionName;
    std::string currentBlockName;
    uint32_t tempCounter = 0;

    std::string nextTemp(const std::string& prefix = "v");
    std::string getLlvmTypeName(Type type) const;
    std::string getAliasScopeMetadata(AliasScopeKind scope) const;
};

} // namespace Llvm
} // namespace CodeGen
} // namespace Luau
