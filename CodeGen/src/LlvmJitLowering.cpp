// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "LlvmJit.h"

#include "Luau/Bytecode.h"
#include "Luau/CodeGenCommon.h"
#include "Luau/Common.h"
#include "Luau/LlvmEngine.h"

#include "NativeState.h"
#include "CodeGenUtils.h"

#include "lstate.h"
#include "lobject.h"
#include "lgc.h"

#include <climits>
#include <cmath>

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#if LUAU_USE_LLVM

LUAU_FASTFLAG(LuauBackedgeHeapCheck)

namespace Luau
{
namespace CodeGen
{
namespace
{

// Targeted LLVM using-declarations: `using namespace llvm` would clash with
// the VM's global `Value` union from lobject.h.
using llvm::BasicBlock;
using llvm::ConstantExpr;
using llvm::ConstantFP;
using llvm::Function;
using llvm::FunctionCallee;
using llvm::FunctionType;
using llvm::GlobalValue;
using llvm::GlobalVariable;
using llvm::IRBuilder;
using llvm::LLVMContext;
using llvm::Module;
using llvm::raw_string_ostream;
using llvm::SwitchInst;
using llvm::Type;
using llvm::Value;
using llvm::verifyFunction;
using llvm::verifyModule;

// AUX word of a two-word instruction (second source register / constant index)
uint32_t auxOf(const Proto* proto, uint32_t i)
{
    return proto->code[i + 1];
}

// Words consumed by the instruction at `index` (in 32-bit word units).
// Derived from the encoding table in Luau/Bytecode.h. NEWCLOSURE is variable:
// 1 header word plus one CAPTURE word per upvalue of the child proto.
uint32_t instructionWords(const Proto* proto, uint32_t insn)
{
    const uint8_t op = LUAU_INSN_OP(insn);

    switch (op)
    {
    // header + AUX word
    case LOP_GETGLOBAL:
    case LOP_SETGLOBAL:
    case LOP_GETIMPORT:
    case LOP_GETTABLEKS:
    case LOP_SETTABLEKS:
    case LOP_NAMECALL:
    case LOP_NEWTABLE:
    case LOP_SETLIST:
    case LOP_FORGLOOP:
    case LOP_FASTCALL3:
    case LOP_FASTCALL2:
    case LOP_FASTCALL2K:
    case LOP_LOADKX:
    case LOP_JUMPIFEQ:
    case LOP_JUMPIFLE:
    case LOP_JUMPIFLT:
    case LOP_JUMPIFNOTEQ:
    case LOP_JUMPIFNOTLE:
    case LOP_JUMPIFNOTLT:
    case LOP_JUMPXEQKNIL:
    case LOP_JUMPXEQKB:
    case LOP_JUMPXEQKN:
    case LOP_JUMPXEQKS:
    case LOP_GETUDATAKS:
    case LOP_SETUDATAKS:
    case LOP_NAMECALLUDATA:
    case LOP_NEWCLASSMEMBER:
    case LOP_CALLFB:
    case LOP_CMPPROTO:
    case LOP_NEWCLASS:
        return 2;

    case LOP_NEWCLOSURE:
    {
        const uint32_t child = uint32_t(LUAU_INSN_D(insn));
        CODEGEN_ASSERT(child < uint32_t(proto->sizep));
        return 1 + uint32_t(proto->p[child]->nups);
    }

    case LOP_DUPCLOSURE:
    {
        const uint32_t constant = uint32_t(LUAU_INSN_D(insn));
        if (constant >= uint32_t(proto->sizek) || !ttisfunction(&proto->k[constant]))
            return 1;
        return 1 + uint32_t(clvalue(&proto->k[constant])->nupvalues);
    }

    default:
        return 1;
    }
}

// Lowers one proto to a dso_local function with one resume block per bytecode
// word. Wave 1 fast paths are emitted inline; every other instruction falls
// back to the VM (savedpc is set to the instruction, the function returns 1).
//
// The register file is the VM's own stack (L->ci->base); the native code
// reads and writes TValues in place. Paths that need GC, invoke a callback,
// allocate, or can unwind defer to the VM, so the base loaded at entry stays
// valid for the whole native slice.
class Lowerer
{
public:
    Lowerer(LLVMContext& ctx)
        : B(ctx)
        , i1Ty(B.getInt1Ty())
        , i32Ty(B.getInt32Ty())
        , i64Ty(B.getInt64Ty())
        , dblTy(B.getDoubleTy())
        , ptrTy(B.getPtrTy())
        , offLStateGlobal(offsetof(lua_State, global))
        , offLStateCi(offsetof(lua_State, ci))
        , offLStateBase(offsetof(lua_State, base))
        , offLStateTop(offsetof(lua_State, top))
        , offLStateStackLast(offsetof(lua_State, stack_last))
        , offLStateEndCi(offsetof(lua_State, end_ci))
        , offLStateOpenUpval(offsetof(lua_State, openupval))
        , offCiBase(offsetof(CallInfo, base))
        , offCiFunc(offsetof(CallInfo, func))
        , offCiTop(offsetof(CallInfo, top))
        , offCiSavedPc(offsetof(CallInfo, savedpc))
        , offCiNResults(offsetof(CallInfo, nresults))
        , offCiFlags(offsetof(CallInfo, flags))
        , offProtoCode(offsetof(Proto, code))
        , offProtoK(offsetof(Proto, k))
        , offProtoP(offsetof(Proto, p))
        , offProtoNups(offsetof(Proto, nups))
        , offProtoMaxStackSize(offsetof(Proto, maxstacksize))
        , offProtoNumParams(offsetof(Proto, numparams))
        , offProtoIsVararg(offsetof(Proto, is_vararg))
        , offProtoExecData(offsetof(Proto, execdata))
        , offProtoExecTarget(offsetof(Proto, exectarget))
        , offProtoFunId(offsetof(Proto, funid))
        // Native CallInfo savedpc always starts inside p->code; codeentry may
        // point at the out-of-proto NATIVECALL trampoline.
        , offProtoCodeEntry(offsetof(Proto, code))
        , offProtoDebugInsn(offsetof(Proto, debuginsn))
        , offGlobalCb(offsetof(global_State, cb))
        , offGlobalTotalBytes(offsetof(global_State, totalbytes))
        , offGlobalGcThreshold(offsetof(global_State, GCthreshold))
        , offCbInterrupt(offsetof(lua_Callbacks, interrupt))
        , offTableSizeArray(offsetof(LuaTable, sizearray))
        , offTableReadonly(offsetof(LuaTable, readonly))
        , offTableSafeEnv(offsetof(LuaTable, safeenv))
        , offTableMetatable(offsetof(LuaTable, metatable))
        , offTableArray(offsetof(LuaTable, array))
        , offTableNode(offsetof(LuaTable, node))
        , offTableNodeMask8(offsetof(LuaTable, nodemask8))
        , offNodeKey(offsetof(LuaNode, key))
        , offStringLen(offsetof(TString, len))
        , offClosureUprefs(offsetof(Closure, l.uprefs))
        , offUpValValue(offsetof(UpVal, v))
        , offClosureIsC(offsetof(Closure, isC))
        , offClosureStackSize(offsetof(Closure, stacksize))
        , offClosureEnv(offsetof(Closure, env))
        , offClosureProto(offsetof(Closure, l.p))
    {
    }

    bool lowerModule(Module& module, const std::vector<const Proto*>& protos, std::string& error)
    {
        if (!lowerGate(module, error))
            return false;

        for (const Proto* proto : protos)
        {
            if (!lowerProto(module, proto, error))
                return false;
        }

        return true;
    }

private:
    IRBuilder<> B;

    Type* i1Ty = nullptr;
    Type* i32Ty = nullptr;
    Type* i64Ty = nullptr;
    Type* dblTy = nullptr;
    Type* ptrTy = nullptr;

    uint64_t offLStateGlobal, offLStateCi, offLStateBase, offLStateTop, offLStateStackLast, offLStateEndCi, offLStateOpenUpval;
    uint64_t offCiBase, offCiFunc, offCiTop, offCiSavedPc, offCiNResults, offCiFlags;
    uint64_t offProtoCode, offProtoK, offProtoP, offProtoNups, offProtoMaxStackSize, offProtoNumParams, offProtoIsVararg, offProtoExecData,
        offProtoExecTarget, offProtoFunId, offProtoCodeEntry, offProtoDebugInsn;
    uint64_t offGlobalCb, offGlobalTotalBytes, offGlobalGcThreshold;
    uint64_t offCbInterrupt;
    uint64_t offTableSizeArray, offTableReadonly, offTableSafeEnv, offTableMetatable, offTableArray, offTableNode, offTableNodeMask8, offNodeKey;
    uint64_t offStringLen;
    uint64_t offClosureUprefs, offUpValValue, offClosureIsC, offClosureStackSize, offClosureEnv, offClosureProto;

    // (ptr, ptr, ptr, ptr) -> i32 : (L, proto, target, ctx)
    Function* makeFunction(Module& module, const std::string& name)
    {
        FunctionType* ft = FunctionType::get(B.getInt32Ty(), {ptrTy, ptrTy, ptrTy, ptrTy}, false);
        Function* fn = Function::Create(ft, GlobalValue::ExternalLinkage, name, &module);

        // dso_local (not internal): the VM resolves the entries through the
        // object symbol table, so nothing in the module references them and
        // internal functions would be dead-stripped before object emission.
        fn->setDSOLocal(true);
        return fn;
    }

    bool lowerGate(Module& module, std::string& error)
    {
        Function* fn = makeFunction(module, "luau_jit_gate");
        FunctionType* ft = FunctionType::get(B.getInt32Ty(), {ptrTy, ptrTy, ptrTy, ptrTy}, false);

        BasicBlock* entry = BasicBlock::Create(B.getContext(), "entry", fn);
        B.SetInsertPoint(entry);

        Value* L = fn->getArg(0);
        Value* p = fn->getArg(1);
        Value* target = fn->getArg(2);
        Value* ctx = fn->getArg(3);

        Value* result = B.CreateCall(ft, target, {L, p, target, ctx});
        B.CreateRet(result);

        std::string verifyMsg;
        raw_string_ostream os(verifyMsg);
        if (verifyFunction(*fn, &os))
        {
            error = "gate verification failed: " + verifyMsg;
            return false;
        }

        return true;
    }

    // --- address and TValue helpers ------------------------------------------

    Value* addrOf(Value* base, uint64_t offset)
    {
        return B.CreateAdd(B.CreatePtrToInt(base, i64Ty), B.getInt64(offset));
    }

    Value* addrOf(Value* base, Value* offset)
    {
        return B.CreateAdd(B.CreatePtrToInt(base, i64Ty), offset);
    }

    Value* toPtr(Value* addr)
    {
        return B.CreateIntToPtr(addr, ptrTy);
    }

    Value* contextFunction(Value* context, uint64_t offset, const char* name)
    {
        return B.CreateLoad(ptrTy, toPtr(addrOf(context, offset)), name);
    }

    Value* regPtrOf(Value* base, int reg)
    {
        // reg is a compile-time constant (8-bit bytecode field)
        return toPtr(B.CreateAdd(B.CreatePtrToInt(base, i64Ty), B.getInt64(int64_t(reg) * sizeof(TValue)), "reg"));
    }

    Value* loadTT(Value* tv)
    {
        return B.CreateLoad(i32Ty, toPtr(addrOf(tv, offsetof(TValue, tt))));
    }

    Value* loadVal(Value* tv)
    {
        return B.CreateLoad(i64Ty, tv);
    }

    Value* loadNum(Value* tv)
    {
        return B.CreateBitCast(B.CreateLoad(i64Ty, tv), dblTy);
    }

    void storeNil(Value* tv)
    {
        B.CreateStore(B.getInt64(0), tv);
        B.CreateStore(B.getInt32(0), toPtr(addrOf(tv, offsetof(TValue, extra))));
        B.CreateStore(B.getInt32(LUA_TNIL), toPtr(addrOf(tv, offsetof(TValue, tt))));
    }

    void storeNum(Value* tv, Value* value)
    {
        B.CreateStore(B.CreateBitCast(value, i64Ty), tv);
        B.CreateStore(B.getInt32(LUA_TNUMBER), toPtr(addrOf(tv, offsetof(TValue, tt))));
    }

    void storeBool(Value* tv, Value* value)
    {
        B.CreateStore(B.CreateZExt(value, i32Ty), tv);
        B.CreateStore(B.getInt32(LUA_TBOOLEAN), toPtr(addrOf(tv, offsetof(TValue, tt))));
    }

    void copyTV(Value* dst, Value* src)
    {
        B.CreateStore(B.CreateLoad(i64Ty, src), dst);
        B.CreateStore(B.CreateLoad(i32Ty, toPtr(addrOf(src, offsetof(TValue, extra)))), toPtr(addrOf(dst, offsetof(TValue, extra))));
        B.CreateStore(B.CreateLoad(i32Ty, toPtr(addrOf(src, offsetof(TValue, tt)))), toPtr(addrOf(dst, offsetof(TValue, tt))));
    }

    Value* isFalse(Value* tv)
    {
        Value* tt = loadTT(tv);
        Value* val = loadVal(tv);
        Value* nil = B.CreateICmpEQ(tt, B.getInt32(LUA_TNIL), "isnil");
        Value* boolean = B.CreateICmpEQ(tt, B.getInt32(LUA_TBOOLEAN), "isbool");
        Value* zero = B.CreateICmpEQ(val, B.getInt64(0), "iszero");
        return B.CreateOr(nil, B.CreateAnd(boolean, zero), "isfalse");
    }

    // libm call through the external pointer table (the JIT object must not
    // reference undefined symbols; libm lives in the host process)
    FunctionType* dblFn1Ty()
    {
        return FunctionType::get(dblTy, {dblTy}, false);
    }

    FunctionType* dblFn2Ty()
    {
        return FunctionType::get(dblTy, {dblTy, dblTy}, false);
    }

    // --- per-proto lowering ---------------------------------------------------

    bool lowerProto(Module& module, const Proto* proto, std::string& error)
    {
        const uint32_t sizecode = proto->sizecode;
        const std::string name = llvmJitEntrySymbolName(uint32_t(proto->bytecodeid));

        Function* fn = makeFunction(module, name);

        Value* L = fn->getArg(0);
        Value* p = fn->getArg(1);
        Value* nativeContext = fn->getArg(3);

        // entry: compute the resume index from L->ci->savedpc
        BasicBlock* entry = BasicBlock::Create(B.getContext(), "entry", fn);
        B.SetInsertPoint(entry);

        Value* fbSlot = B.CreateAlloca(i64Ty, nullptr, "fbidx");
        B.CreateStore(B.getInt64(0), fbSlot);

        Value* ci = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "ci");
        Value* base = B.CreateLoad(ptrTy, toPtr(addrOf(ci, offCiBase)), "base");
        Value* code = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoCode)), "code");
        Value* savedPc = B.CreateLoad(ptrTy, toPtr(addrOf(ci, offCiSavedPc)), "savedpc");

        Value* idx =
            B.CreateSDiv(B.CreateSub(B.CreatePtrToInt(savedPc, i64Ty), B.CreatePtrToInt(code, i64Ty)), B.getInt64(sizeof(Instruction)), "idx");
        Value* inRange = B.CreateICmpSLT(idx, B.getInt64(sizecode), "inrange");

        BasicBlock* dispatch = BasicBlock::Create(B.getContext(), "dispatch", fn);
        BasicBlock* fallback = BasicBlock::Create(B.getContext(), "fallback", fn);
        BasicBlock* exitNoop = BasicBlock::Create(B.getContext(), "exit", fn);
        BasicBlock* debugCheck = BasicBlock::Create(B.getContext(), "debugcheck", fn);
        B.CreateStore(idx, fbSlot);
        B.CreateCondBr(inRange, debugCheck, exitNoop);

        // Debugger breakpoints patch live bytecode after compilation.  Keep
        // the whole invocation in the VM whenever that patch table exists.
        B.SetInsertPoint(debugCheck);
        Value* debugInstructions = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoDebugInsn)), "debuginsn");
        B.CreateCondBr(B.CreateIsNull(debugInstructions), dispatch, fallback);

        // one resume block per bytecode word
        std::vector<BasicBlock*> blocks(sizecode);
        for (uint32_t i = 0; i < sizecode; ++i)
            blocks[i] = BasicBlock::Create(B.getContext(), "insn" + std::to_string(i), fn);

        B.SetInsertPoint(dispatch);
        SwitchInst* sw = B.CreateSwitch(idx, fallback, sizecode);
        for (uint32_t i = 0; i < sizecode; ++i)
            sw->addCase(B.getInt64(i), blocks[i]);

        // fallback: resume in the VM at the instruction recorded in fbSlot
        B.SetInsertPoint(fallback);
        Value* fbIdx = B.CreateLoad(i64Ty, fbSlot, "fbidx");
        Value* fbInRange = B.CreateICmpULT(fbIdx, B.getInt64(sizecode), "fb_inrange");
        BasicBlock* fbStore = BasicBlock::Create(B.getContext(), "fb_store", fn);
        BasicBlock* fbRet = BasicBlock::Create(B.getContext(), "fb_ret", fn);
        B.CreateCondBr(fbInRange, fbStore, fbRet);

        B.SetInsertPoint(fbStore);
        Value* fbPc = toPtr(addrOf(code, B.CreateMul(fbIdx, B.getInt64(sizeof(Instruction)))));
        B.CreateStore(fbPc, toPtr(addrOf(ci, offCiSavedPc)));
        B.CreateBr(fbRet);

        B.SetInsertPoint(fbRet);
        B.CreateRet(B.getInt32(1));

        B.SetInsertPoint(exitNoop);
        B.CreateRet(B.getInt32(1));

        auto fb = [&](uint32_t i)
        {
            B.CreateStore(B.getInt64(i), fbSlot);
            B.CreateBr(fallback);
        };

        // Resolve a relative jump target (VM pc semantics: i + 1 + delta).
        // Returns the target block, or nullptr after emitting the fallback.
        auto jumpTo = [&](uint32_t i, int32_t delta, uint32_t next) -> BasicBlock*
        {
            const int64_t target = int64_t(i) + 1 + int64_t(delta);

            if (target >= 0 && target < int64_t(sizecode))
                return blocks[uint32_t(target)];

            // defensive: malformed jump target falls back to the VM
            fb(i);
            return nullptr;
        };

        // Binary floating-point operation with the both-numeric fast path.
        // The VM's double arithmetic is plain IEEE (luai_num*), so LLVM's
        // fadd/fsub/fmul/fdiv/fpow/floor lower to exactly the same operations.
        enum class BinOp
        {
            Add,
            Sub,
            Mul,
            Div,
            IDiv,
            Mod,
            Pow
        };

        auto lowerBinOp = [&](uint32_t i, uint32_t a, uint32_t b, uint32_t c, BinOp kind, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* rb = regPtrOf(base, int(b));
            Value* rc = regPtrOf(base, int(c));

            Value* ttB = loadTT(rb);
            Value* ttC = loadTT(rc);
            Value* fast = B.CreateAnd(B.CreateICmpEQ(ttB, B.getInt32(LUA_TNUMBER)), B.CreateICmpEQ(ttC, B.getInt32(LUA_TNUMBER)), "fast");

            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
            B.CreateCondBr(fast, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            Value* nb = loadNum(rb);
            Value* nc = loadNum(rc);
            Value* r;
            switch (kind)
            {
            case BinOp::Add:
                r = B.CreateFAdd(nb, nc);
                break;
            case BinOp::Sub:
                r = B.CreateFSub(nb, nc);
                break;
            case BinOp::Mul:
                r = B.CreateFMul(nb, nc);
                break;
            case BinOp::Div:
                r = B.CreateFDiv(nb, nc);
                break;
            case BinOp::IDiv:
            {
                Value* flFn = ConstantExpr::getIntToPtr(B.getInt64(reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(::floor))), ptrTy);
                r = B.CreateCall(dblFn1Ty(), flFn, {B.CreateFDiv(nb, nc)});
                break;
            }
            case BinOp::Mod:
            {
                // luai_nummod: a - floor(a / b) * b
                Value* flFn = ConstantExpr::getIntToPtr(B.getInt64(reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(::floor))), ptrTy);
                Value* fl = B.CreateCall(dblFn1Ty(), flFn, {B.CreateFDiv(nb, nc)});
                r = B.CreateFSub(nb, B.CreateFMul(fl, nc));
                break;
            }
            default:
            {
                // Pow: libm pow, exactly as the VM (luai_numpow)
                Value* pwFn =
                    ConstantExpr::getIntToPtr(B.getInt64(reinterpret_cast<uintptr_t>(static_cast<double (*)(double, double)>(::pow))), ptrTy);
                r = B.CreateCall(dblFn2Ty(), pwFn, {nb, nc});
                break;
            }
            }
            storeNum(regPtrOf(base, int(a)), r);
            B.CreateBr(blocks[next]);
        };

        // rA = rB <op> kC (or kC <op> rB for the RK variants); both-numeric fast path
        auto lowerBinOpK = [&](uint32_t i, uint32_t next)
        {
            const uint8_t op = LUAU_INSN_OP(proto->code[i]);
            const bool constFirst = (op == LOP_SUBRK || op == LOP_DIVRK);
            const uint32_t a = LUAU_INSN_A(proto->code[i]);
            const uint32_t breg = constFirst ? LUAU_INSN_C(proto->code[i]) : LUAU_INSN_B(proto->code[i]);
            const uint32_t cidx = constFirst ? LUAU_INSN_B(proto->code[i]) : LUAU_INSN_C(proto->code[i]);

            B.SetInsertPoint(blocks[i]);
            Value* rb = regPtrOf(base, int(breg));
            Value* k = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            Value* kv = toPtr(addrOf(k, uint64_t(cidx) * sizeof(TValue)));

            Value* ttR = loadTT(rb);
            Value* ttK = loadTT(kv);
            Value* fast = B.CreateAnd(B.CreateICmpEQ(ttR, B.getInt32(LUA_TNUMBER)), B.CreateICmpEQ(ttK, B.getInt32(LUA_TNUMBER)), "fast");

            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
            B.CreateCondBr(fast, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            Value* nr = loadNum(rb);
            Value* nk = loadNum(kv);
            Value* r;
            switch (op)
            {
            case LOP_ADDK:
                r = B.CreateFAdd(nk, nr);
                break;
            case LOP_SUBK:
                r = B.CreateFSub(nr, nk);
                break;
            case LOP_MULK:
                r = B.CreateFMul(nr, nk);
                break;
            case LOP_DIVK:
                r = B.CreateFDiv(nr, nk);
                break;
            case LOP_IDIVK:
            {
                Value* flFn = ConstantExpr::getIntToPtr(B.getInt64(reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(::floor))), ptrTy);
                r = B.CreateCall(dblFn1Ty(), flFn, {B.CreateFDiv(nr, nk)});
                break;
            }
            case LOP_MODK:
            {
                Value* flFn = ConstantExpr::getIntToPtr(B.getInt64(reinterpret_cast<uintptr_t>(static_cast<double (*)(double)>(::floor))), ptrTy);
                Value* fl = B.CreateCall(dblFn1Ty(), flFn, {B.CreateFDiv(nr, nk)});
                r = B.CreateFSub(nr, B.CreateFMul(fl, nk));
                break;
            }
            case LOP_POWK:
            {
                Value* pwFn =
                    ConstantExpr::getIntToPtr(B.getInt64(reinterpret_cast<uintptr_t>(static_cast<double (*)(double, double)>(::pow))), ptrTy);
                r = B.CreateCall(dblFn2Ty(), pwFn, {nr, nk});
                break;
            }
            case LOP_SUBRK:
                r = B.CreateFSub(nk, nr);
                break;
            default: // LOP_DIVRK
                r = B.CreateFDiv(nk, nr);
                break;
            }
            storeNum(regPtrOf(base, int(a)), r);
            B.CreateBr(blocks[next]);
        };

        auto lowerTruthJump = [&](uint32_t i, uint32_t a, int32_t delta, uint32_t next, bool invert)
        {
            Value* ra = regPtrOf(base, int(a));
            Value* jump = B.CreateNot(isFalse(ra));
            if (invert)
                jump = B.CreateNot(jump);
            if (delta == 0)
                B.CreateBr(blocks[next]);
            else
            {
                BasicBlock* target = jumpTo(i, delta, next);
                if (target)
                    B.CreateCondBr(jump, target, blocks[next]);
            }
        };

        auto lowerCmpJump = [&](uint32_t i, uint32_t a, uint32_t b, int32_t delta, uint32_t next, bool invert, bool isEq, bool strict)
        {
            CODEGEN_ASSERT(i + 1 < sizecode);

            B.SetInsertPoint(blocks[i]);
            Value* ra = regPtrOf(base, int(a));
            Value* rb = regPtrOf(base, int(b));

            Value* ttA = loadTT(ra);
            Value* ttB = loadTT(rb);
            Value* valA = loadVal(ra);
            Value* valB = loadVal(rb);

            Value* result;
            BasicBlock* fastBlock = nullptr;

            if (isEq)
            {
                // Fast path: same tag with trivial equality (nil, booleans,
                // integers, light userdata, same GC object), or any two
                // numbers. Different tags may still be equal through __eq.
                Value* bothNil = B.CreateAnd(B.CreateICmpEQ(ttA, B.getInt32(LUA_TNIL)), B.CreateICmpEQ(ttB, B.getInt32(LUA_TNIL)), "bothnil");
                Value* sameTag = B.CreateICmpEQ(ttA, ttB, "samett");
                Value* sameVal = B.CreateICmpEQ(valA, valB, "sameval");
                Value* trivial = B.CreateAnd(sameTag, sameVal, "trivial");
                Value* bothNum = B.CreateAnd(B.CreateICmpEQ(ttA, B.getInt32(LUA_TNUMBER)), B.CreateICmpEQ(ttB, B.getInt32(LUA_TNUMBER)), "bothnum");

                fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
                B.CreateCondBr(B.CreateOr(B.CreateOr(bothNil, trivial), bothNum), fastBlock, fallback);

                B.SetInsertPoint(fastBlock);
                Value* eqNum = B.CreateFCmpOEQ(B.CreateBitCast(valA, dblTy), B.CreateBitCast(valB, dblTy), "eqnum");
                result = B.CreateSelect(bothNum, eqNum, B.getInt1(1), "eq");
            }
            else
            {
                // Fast path: both numbers (mirrors the VM's first fast path;
                // integers and mixed tags route through luaV_lessthan in the VM)
                Value* bothNum = B.CreateAnd(B.CreateICmpEQ(ttA, B.getInt32(LUA_TNUMBER)), B.CreateICmpEQ(ttB, B.getInt32(LUA_TNUMBER)), "bothnum");

                fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
                B.CreateCondBr(bothNum, fastBlock, fallback);

                B.SetInsertPoint(fastBlock);
                Value* da = B.CreateBitCast(valA, dblTy);
                Value* db = B.CreateBitCast(valB, dblTy);
                result = strict ? B.CreateFCmpOLT(da, db, "cmp") : B.CreateFCmpOLE(da, db, "cmp");
            }

            if (invert)
                result = B.CreateNot(result);

            // no-jump for two-word instructions skips the AUX word (delta 1)
            if (delta == 1)
                B.CreateBr(blocks[next]);
            else
            {
                BasicBlock* target = jumpTo(i, delta, next);
                if (target)
                    B.CreateCondBr(result, target, blocks[next]);
            }
        };

        // Long equality jumps compare against a primitive encoded in their
        // AUX word.  They never invoke __eq: non-matching tags simply fail
        // the comparison, exactly like the VM opcode handlers.
        auto lowerJumpXEqK = [&](uint32_t i, uint32_t next)
        {
            const uint8_t op = LUAU_INSN_OP(proto->code[i]);
            const uint32_t aux = auxOf(proto, i);
            const int32_t delta = LUAU_INSN_D(proto->code[i]);

            B.SetInsertPoint(blocks[i]);
            Value* ra = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* tag = loadTT(ra);
            Value* matches = nullptr;

            switch (op)
            {
            case LOP_JUMPXEQKNIL:
                matches = B.CreateICmpEQ(tag, B.getInt32(LUA_TNIL), "eqnil");
                break;
            case LOP_JUMPXEQKB:
                matches = B.CreateAnd(
                    B.CreateICmpEQ(tag, B.getInt32(LUA_TBOOLEAN)),
                    // TValue::value.b is an int; its upper four bytes are
                    // unspecified, so do not compare the whole value union.
                    B.CreateICmpEQ(B.CreateLoad(i32Ty, ra), B.getInt32(LUAU_INSN_AUX_KB(aux))),
                    "eqbool"
                );
                break;
            case LOP_JUMPXEQKN:
            {
                Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
                Value* constant = toPtr(addrOf(constants, uint64_t(LUAU_INSN_AUX_KV(aux)) * sizeof(TValue)));
                matches = B.CreateAnd(B.CreateICmpEQ(tag, B.getInt32(LUA_TNUMBER)), B.CreateFCmpOEQ(loadNum(ra), loadNum(constant)), "eqnum");
                break;
            }
            case LOP_JUMPXEQKS:
            {
                Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
                Value* constant = toPtr(addrOf(constants, uint64_t(LUAU_INSN_AUX_KV(aux)) * sizeof(TValue)));
                matches = B.CreateAnd(B.CreateICmpEQ(tag, B.getInt32(LUA_TSTRING)), B.CreateICmpEQ(loadVal(ra), loadVal(constant)), "eqstr");
                break;
            }
            default:
                CODEGEN_ASSERT(false);
                return;
            }

            Value* jump = LUAU_INSN_AUX_NOT(aux) ? B.CreateNot(matches) : matches;
            if (delta == 1)
                B.CreateBr(blocks[next]);
            else
            {
                BasicBlock* target = jumpTo(i, delta, next);
                if (target)
                    B.CreateCondBr(jump, target, blocks[next]);
            }
        };

        // Cached string lookup shared by GETGLOBAL and GETTABLEKS.  The VM
        // encodes the predicted node in C and masks it by nodemask8.  A hit
        // requires the exact interned string pointer and a non-nil value.
        auto lowerCachedStringGet = [&](uint32_t i, uint32_t next, Value* table, Value* destination, Value* constant)
        {
            Value* mask = B.CreateZExt(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(table, offTableNodeMask8))), i64Ty, "nodemask");
            Value* slot = B.CreateAnd(B.getInt64(LUAU_INSN_C(proto->code[i])), mask, "slot");
            Value* nodes = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableNode)), "nodes");
            Value* node = toPtr(addrOf(nodes, B.CreateMul(slot, B.getInt64(sizeof(LuaNode)))));
            Value* key = toPtr(addrOf(node, offNodeKey));

            constexpr uint64_t keyTagWord = offsetof(TKey, extra) + sizeof(((TKey*)nullptr)->extra);
            Value* packedKeyTag = B.CreateLoad(i32Ty, toPtr(addrOf(key, keyTagWord)), "keytagword");
            Value* keyTag = B.CreateAnd(packedKeyTag, B.getInt32(0xf), "keytag");
            Value* keyMatches =
                B.CreateAnd(B.CreateICmpEQ(keyTag, B.getInt32(LUA_TSTRING)), B.CreateICmpEQ(loadVal(key), loadVal(constant)), "keymatch");
            Value* valuePresent = B.CreateICmpNE(loadTT(node), B.getInt32(LUA_TNIL), "present");
            BasicBlock* hit = BasicBlock::Create(B.getContext(), "cachehit", fn);
            B.CreateCondBr(B.CreateAnd(keyMatches, valuePresent), hit, fallback);

            B.SetInsertPoint(hit);
            copyTV(destination, node);
            B.CreateBr(blocks[next]);
        };

        auto lowerGetGlobal = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            Value* constant = toPtr(addrOf(constants, uint64_t(auxOf(proto, i)) * sizeof(TValue)));
            Value* function = toPtr(addrOf(ci, offCiFunc));
            Value* closure = toPtr(loadVal(function));
            Value* env = B.CreateLoad(ptrTy, toPtr(addrOf(closure, offClosureEnv)), "env");
            lowerCachedStringGet(i, next, env, regPtrOf(base, int(LUAU_INSN_A(proto->code[i]))), constant);
        };

        auto lowerGetTableKs = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* tableValue = regPtrOf(base, int(LUAU_INSN_B(proto->code[i])));
            Value* isTable = B.CreateICmpEQ(loadTT(tableValue), B.getInt32(LUA_TTABLE), "istable");
            BasicBlock* tableBlock = BasicBlock::Create(B.getContext(), "tableks", fn);
            B.CreateCondBr(isTable, tableBlock, fallback);

            B.SetInsertPoint(tableBlock);
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            Value* constant = toPtr(addrOf(constants, uint64_t(auxOf(proto, i)) * sizeof(TValue)));
            lowerCachedStringGet(i, next, toPtr(loadVal(tableValue)), regPtrOf(base, int(LUAU_INSN_A(proto->code[i]))), constant);
        };

        // Cached string writes can stay native only for primitive values;
        // collectable values require luaC_barriert and therefore use the VM.
        auto lowerCachedStringSet = [&](uint32_t i, uint32_t next, Value* table, Value* source, Value* constant)
        {
            Value* mask = B.CreateZExt(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(table, offTableNodeMask8))), i64Ty, "nodemask");
            Value* slot = B.CreateAnd(B.getInt64(LUAU_INSN_C(proto->code[i])), mask, "slot");
            Value* nodes = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableNode)), "nodes");
            Value* node = toPtr(addrOf(nodes, B.CreateMul(slot, B.getInt64(sizeof(LuaNode)))));
            Value* key = toPtr(addrOf(node, offNodeKey));
            constexpr uint64_t keyTagWord = offsetof(TKey, extra) + sizeof(((TKey*)nullptr)->extra);
            Value* packedKeyTag = B.CreateLoad(i32Ty, toPtr(addrOf(key, keyTagWord)), "keytagword");
            Value* keyTag = B.CreateAnd(packedKeyTag, B.getInt32(0xf), "keytag");
            Value* keyMatches =
                B.CreateAnd(B.CreateICmpEQ(keyTag, B.getInt32(LUA_TSTRING)), B.CreateICmpEQ(loadVal(key), loadVal(constant)), "keymatch");
            Value* valuePresent = B.CreateICmpNE(loadTT(node), B.getInt32(LUA_TNIL), "present");
            Value* writable = B.CreateICmpEQ(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(table, offTableReadonly))), B.getInt8(0), "writable");
            Value* primitive = B.CreateICmpULT(loadTT(source), B.getInt32(LUA_TSTRING), "primitive");
            Value* eligible = B.CreateAnd(B.CreateAnd(keyMatches, valuePresent), B.CreateAnd(writable, primitive), "cachewrite");
            BasicBlock* hit = BasicBlock::Create(B.getContext(), "cachewrite", fn);
            B.CreateCondBr(eligible, hit, fallback);

            B.SetInsertPoint(hit);
            copyTV(node, source);
            B.CreateBr(blocks[next]);
        };

        auto lowerSetGlobal = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            Value* constant = toPtr(addrOf(constants, uint64_t(auxOf(proto, i)) * sizeof(TValue)));
            Value* function = toPtr(addrOf(ci, offCiFunc));
            Value* closure = toPtr(loadVal(function));
            Value* env = B.CreateLoad(ptrTy, toPtr(addrOf(closure, offClosureEnv)), "env");
            lowerCachedStringSet(i, next, env, regPtrOf(base, int(LUAU_INSN_A(proto->code[i]))), constant);
        };

        auto lowerSetTableKs = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* tableValue = regPtrOf(base, int(LUAU_INSN_B(proto->code[i])));
            Value* isTable = B.CreateICmpEQ(loadTT(tableValue), B.getInt32(LUA_TTABLE), "istable");
            BasicBlock* tableBlock = BasicBlock::Create(B.getContext(), "settableks", fn);
            B.CreateCondBr(isTable, tableBlock, fallback);

            B.SetInsertPoint(tableBlock);
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            Value* constant = toPtr(addrOf(constants, uint64_t(auxOf(proto, i)) * sizeof(TValue)));
            lowerCachedStringSet(i, next, toPtr(loadVal(tableValue)), regPtrOf(base, int(LUAU_INSN_A(proto->code[i]))), constant);
        };

        // NAMECALL uses the same predicted string slot as GETTABLEKS, then
        // writes the receiver to A+1 before placing the method in A.
        auto lowerNameCall = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* receiver = regPtrOf(base, int(LUAU_INSN_B(proto->code[i])));
            Value* isTable = B.CreateICmpEQ(loadTT(receiver), B.getInt32(LUA_TTABLE), "istable");
            BasicBlock* tableBlock = BasicBlock::Create(B.getContext(), "namecalltable", fn);
            B.CreateCondBr(isTable, tableBlock, fallback);

            B.SetInsertPoint(tableBlock);
            Value* table = toPtr(loadVal(receiver));
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            Value* constant = toPtr(addrOf(constants, uint64_t(auxOf(proto, i)) * sizeof(TValue)));
            Value* mask = B.CreateZExt(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(table, offTableNodeMask8))), i64Ty, "nodemask");
            Value* slot = B.CreateAnd(B.getInt64(LUAU_INSN_C(proto->code[i])), mask, "slot");
            Value* nodes = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableNode)), "nodes");
            Value* node = toPtr(addrOf(nodes, B.CreateMul(slot, B.getInt64(sizeof(LuaNode)))));
            Value* key = toPtr(addrOf(node, offNodeKey));
            constexpr uint64_t keyTagWord = offsetof(TKey, extra) + sizeof(((TKey*)nullptr)->extra);
            Value* packedKeyTag = B.CreateLoad(i32Ty, toPtr(addrOf(key, keyTagWord)), "keytagword");
            Value* keyTag = B.CreateAnd(packedKeyTag, B.getInt32(0xf), "keytag");
            Value* keyMatches =
                B.CreateAnd(B.CreateICmpEQ(keyTag, B.getInt32(LUA_TSTRING)), B.CreateICmpEQ(loadVal(key), loadVal(constant)), "keymatch");
            Value* present = B.CreateICmpNE(loadTT(node), B.getInt32(LUA_TNIL), "present");
            BasicBlock* hit = BasicBlock::Create(B.getContext(), "namecallhit", fn);
            B.CreateCondBr(B.CreateAnd(keyMatches, present), hit, fallback);

            B.SetInsertPoint(hit);
            Value* destination = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            copyTV(toPtr(addrOf(destination, sizeof(TValue))), receiver);
            copyTV(destination, node);
            B.CreateBr(blocks[next]);
        };

        // GETTABLEN has a complete non-metatable array fast path.  Preserve
        // the VM fallback for non-tables, bounds misses, and __index.
        auto lowerGetTableN = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);

            Value* destination = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* tableValue = regPtrOf(base, int(LUAU_INSN_B(proto->code[i])));
            Value* isTable = B.CreateICmpEQ(loadTT(tableValue), B.getInt32(LUA_TTABLE), "istable");

            BasicBlock* tableBlock = BasicBlock::Create(B.getContext(), "table", fn);
            B.CreateCondBr(isTable, tableBlock, fallback);

            B.SetInsertPoint(tableBlock);
            Value* table = toPtr(loadVal(tableValue));
            Value* sizeArray = B.CreateLoad(i32Ty, toPtr(addrOf(table, offTableSizeArray)), "sizearray");
            Value* inBounds = B.CreateICmpUGT(sizeArray, B.getInt32(LUAU_INSN_C(proto->code[i])), "inbounds");
            Value* metatable = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableMetatable)), "metatable");
            Value* plainTable = B.CreateAnd(inBounds, B.CreateIsNull(metatable), "plainarray");

            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
            B.CreateCondBr(plainTable, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            Value* array = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableArray)), "array");
            Value* element = toPtr(addrOf(array, uint64_t(LUAU_INSN_C(proto->code[i])) * sizeof(TValue)));
            copyTV(destination, element);
            B.CreateBr(blocks[next]);
        };

        // Dynamic table indexing shares the GETTABLEN array fast path once
        // the key is proven to be an exact positive integer.  Do not convert
        // an out-of-range double to int: that would be undefined in LLVM and
        // must instead fall back to the VM.
        auto lowerGetTable = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);

            Value* destination = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* tableValue = regPtrOf(base, int(LUAU_INSN_B(proto->code[i])));
            Value* keyValue = regPtrOf(base, int(LUAU_INSN_C(proto->code[i])));
            Value* isTable = B.CreateICmpEQ(loadTT(tableValue), B.getInt32(LUA_TTABLE), "istable");

            BasicBlock* tableBlock = BasicBlock::Create(B.getContext(), "table", fn);
            B.CreateCondBr(isTable, tableBlock, fallback);

            B.SetInsertPoint(tableBlock);
            Value* isNumber = B.CreateICmpEQ(loadTT(keyValue), B.getInt32(LUA_TNUMBER), "isnumber");
            BasicBlock* keyBlock = BasicBlock::Create(B.getContext(), "numberkey", fn);
            B.CreateCondBr(isNumber, keyBlock, fallback);

            B.SetInsertPoint(keyBlock);
            Value* key = loadNum(keyValue);
            Value* inIntRange = B.CreateAnd(
                B.CreateFCmpOGE(key, ConstantFP::get(dblTy, 1.0)), B.CreateFCmpOLE(key, ConstantFP::get(dblTy, double(INT_MAX))), "intrange"
            );
            BasicBlock* indexBlock = BasicBlock::Create(B.getContext(), "index", fn);
            B.CreateCondBr(inIntRange, indexBlock, fallback);

            B.SetInsertPoint(indexBlock);
            Value* index = B.CreateFPToSI(key, i32Ty, "index");
            Value* exactInteger = B.CreateFCmpOEQ(key, B.CreateSIToFP(index, dblTy), "exactindex");
            Value* table = toPtr(loadVal(tableValue));
            Value* sizeArray = B.CreateLoad(i32Ty, toPtr(addrOf(table, offTableSizeArray)), "sizearray");
            Value* inBounds = B.CreateICmpULT(B.CreateSub(index, B.getInt32(1)), sizeArray, "inbounds");
            Value* metatable = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableMetatable)), "metatable");
            Value* plainTable = B.CreateAnd(B.CreateAnd(exactInteger, inBounds), B.CreateIsNull(metatable), "plainarray");

            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
            B.CreateCondBr(plainTable, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            Value* array = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableArray)), "array");
            Value* offset = B.CreateMul(B.CreateZExt(B.CreateSub(index, B.getInt32(1)), i64Ty), B.getInt64(sizeof(TValue)));
            Value* element = toPtr(addrOf(array, offset));
            copyTV(destination, element);
            B.CreateBr(blocks[next]);
        };

        // A table write requires a GC barrier only when storing a collectable
        // value.  Keep those, metatable dispatch, bounds misses, and readonly
        // tables on the VM path; primitive writes to a plain array are exact.
        auto lowerSetTableN = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);

            Value* source = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* tableValue = regPtrOf(base, int(LUAU_INSN_B(proto->code[i])));
            Value* isTable = B.CreateICmpEQ(loadTT(tableValue), B.getInt32(LUA_TTABLE), "istable");

            BasicBlock* tableBlock = BasicBlock::Create(B.getContext(), "table", fn);
            B.CreateCondBr(isTable, tableBlock, fallback);

            B.SetInsertPoint(tableBlock);
            Value* table = toPtr(loadVal(tableValue));
            Value* sizeArray = B.CreateLoad(i32Ty, toPtr(addrOf(table, offTableSizeArray)), "sizearray");
            Value* inBounds = B.CreateICmpUGT(sizeArray, B.getInt32(LUAU_INSN_C(proto->code[i])), "inbounds");
            Value* metatable = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableMetatable)), "metatable");
            Value* writable = B.CreateICmpEQ(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(table, offTableReadonly))), B.getInt8(0), "writable");
            Value* plainTable = B.CreateAnd(B.CreateAnd(inBounds, B.CreateIsNull(metatable)), writable, "plainarray");

            BasicBlock* plainBlock = BasicBlock::Create(B.getContext(), "plain", fn);
            B.CreateCondBr(plainTable, plainBlock, fallback);

            B.SetInsertPoint(plainBlock);
            Value* primitive = B.CreateICmpULT(loadTT(source), B.getInt32(LUA_TSTRING), "primitive");
            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
            B.CreateCondBr(primitive, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            Value* array = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableArray)), "array");
            Value* element = toPtr(addrOf(array, uint64_t(LUAU_INSN_C(proto->code[i])) * sizeof(TValue)));
            copyTV(element, source);
            B.CreateBr(blocks[next]);
        };

        // Dynamic primitive array writes mirror SETTABLEN after proving the
        // key is an exact positive integer.  Collectable writes use the VM so
        // that the required table barrier is never skipped.
        auto lowerSetTable = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);

            Value* source = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* tableValue = regPtrOf(base, int(LUAU_INSN_B(proto->code[i])));
            Value* keyValue = regPtrOf(base, int(LUAU_INSN_C(proto->code[i])));
            Value* isTable = B.CreateICmpEQ(loadTT(tableValue), B.getInt32(LUA_TTABLE), "istable");

            BasicBlock* tableBlock = BasicBlock::Create(B.getContext(), "table", fn);
            B.CreateCondBr(isTable, tableBlock, fallback);

            B.SetInsertPoint(tableBlock);
            Value* isNumber = B.CreateICmpEQ(loadTT(keyValue), B.getInt32(LUA_TNUMBER), "isnumber");
            BasicBlock* keyBlock = BasicBlock::Create(B.getContext(), "numberkey", fn);
            B.CreateCondBr(isNumber, keyBlock, fallback);

            B.SetInsertPoint(keyBlock);
            Value* key = loadNum(keyValue);
            Value* inIntRange = B.CreateAnd(
                B.CreateFCmpOGE(key, ConstantFP::get(dblTy, 1.0)), B.CreateFCmpOLE(key, ConstantFP::get(dblTy, double(INT_MAX))), "intrange"
            );
            BasicBlock* indexBlock = BasicBlock::Create(B.getContext(), "index", fn);
            B.CreateCondBr(inIntRange, indexBlock, fallback);

            B.SetInsertPoint(indexBlock);
            Value* index = B.CreateFPToSI(key, i32Ty, "index");
            Value* exactInteger = B.CreateFCmpOEQ(key, B.CreateSIToFP(index, dblTy), "exactindex");
            Value* table = toPtr(loadVal(tableValue));
            Value* sizeArray = B.CreateLoad(i32Ty, toPtr(addrOf(table, offTableSizeArray)), "sizearray");
            Value* inBounds = B.CreateICmpULT(B.CreateSub(index, B.getInt32(1)), sizeArray, "inbounds");
            Value* metatable = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableMetatable)), "metatable");
            Value* writable = B.CreateICmpEQ(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(table, offTableReadonly))), B.getInt8(0), "writable");
            Value* primitive = B.CreateICmpULT(loadTT(source), B.getInt32(LUA_TSTRING), "primitive");
            Value* plainTable = B.CreateAnd(
                B.CreateAnd(B.CreateAnd(exactInteger, inBounds), B.CreateIsNull(metatable)), B.CreateAnd(writable, primitive), "plainarray"
            );

            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
            B.CreateCondBr(plainTable, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            Value* array = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableArray)), "array");
            Value* offset = B.CreateMul(B.CreateZExt(B.CreateSub(index, B.getInt32(1)), i64Ty), B.getInt64(sizeof(TValue)));
            Value* element = toPtr(addrOf(array, offset));
            copyTV(element, source);
            B.CreateBr(blocks[next]);
        };

        // Fixed SETLIST writes into already allocated array storage.  Keep
        // resizes, multret sources, readonly tables, and collectable values
        // in the VM because they require stack or GC-barrier handling.
        auto lowerSetList = [&](uint32_t i, uint32_t next)
        {
            const uint32_t insn = proto->code[i];
            const int count = int(LUAU_INSN_C(insn)) - 1;
            const uint32_t start = auxOf(proto, i);
            B.SetInsertPoint(blocks[i]);
            if (count < 0 || start == 0 || start > uint32_t(INT_MAX) || uint64_t(start) + uint64_t(count) - 1 > uint64_t(INT_MAX))
            {
                fb(i);
                return;
            }
            BasicBlock* slow = BasicBlock::Create(B.getContext(), "setlist_slow", fn);

            Value* tableValue = regPtrOf(base, int(LUAU_INSN_A(insn)));
            Value* isTable = B.CreateICmpEQ(loadTT(tableValue), B.getInt32(LUA_TTABLE), "istable");
            BasicBlock* tableBlock = BasicBlock::Create(B.getContext(), "setlist_table", fn);
            B.CreateCondBr(isTable, tableBlock, slow);

            B.SetInsertPoint(tableBlock);
            Value* table = toPtr(loadVal(tableValue));
            Value* size = B.CreateLoad(i32Ty, toPtr(addrOf(table, offTableSizeArray)), "sizearray");
            Value* writable = B.CreateICmpEQ(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(table, offTableReadonly))), B.getInt8(0), "writable");
            Value* enough = B.CreateICmpUGE(size, B.getInt32(int(start) + count - 1), "capacity");
            BasicBlock* values = BasicBlock::Create(B.getContext(), "setlist_values", fn);
            B.CreateCondBr(B.CreateAnd(writable, enough), values, slow);

            B.SetInsertPoint(values);
            Value* allPrimitive = B.getInt1(true);
            for (int slot = 0; slot < count; ++slot)
                allPrimitive =
                    B.CreateAnd(allPrimitive, B.CreateICmpULT(loadTT(regPtrOf(base, int(LUAU_INSN_B(insn)) + slot)), B.getInt32(LUA_TSTRING)));
            BasicBlock* fast = BasicBlock::Create(B.getContext(), "setlist_fast", fn);
            B.CreateCondBr(allPrimitive, fast, slow);

            B.SetInsertPoint(fast);
            Value* array = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableArray)), "array");
            for (int slot = 0; slot < count; ++slot)
                copyTV(toPtr(addrOf(array, uint64_t(start - 1 + slot) * sizeof(TValue))), regPtrOf(base, int(LUAU_INSN_B(insn)) + slot));
            B.CreateBr(blocks[next]);

            B.SetInsertPoint(slow);
            Value* pc = toPtr(addrOf(code, uint64_t(i) * sizeof(Instruction)));
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            FunctionType* helperType = FunctionType::get(ptrTy, {ptrTy, ptrTy, ptrTy, ptrTy}, false);
            Value* helper = contextFunction(nativeContext, offsetof(NativeContext, executeSETLIST), "executeSETLIST");
            Value* resume = B.CreateCall(helperType, helper, {L, pc, base, constants}, "setlist_resume");
            BasicBlock* resumeBlock = BasicBlock::Create(B.getContext(), "setlist_resume", fn);
            BasicBlock* stopBlock = BasicBlock::Create(B.getContext(), "setlist_stop", fn);
            B.CreateCondBr(B.CreateIsNull(resume), stopBlock, resumeBlock);

            B.SetInsertPoint(resumeBlock);
            Value* currentCi = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "currentci");
            B.CreateStore(resume, toPtr(addrOf(currentCi, offCiSavedPc)));
            B.CreateRet(B.getInt32(1));

            B.SetInsertPoint(stopBlock);
            B.CreateRet(B.getInt32(0));
        };

        // String length is a field load. Plain-table length uses luaH_getn;
        // metatable and userdata cases keep their protected VM semantics.
        auto lowerLength = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* destination = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* source = regPtrOf(base, int(LUAU_INSN_B(proto->code[i])));
            Value* isString = B.CreateICmpEQ(loadTT(source), B.getInt32(LUA_TSTRING), "isstring");

            BasicBlock* stringBlock = BasicBlock::Create(B.getContext(), "string", fn);
            BasicBlock* tableCheck = BasicBlock::Create(B.getContext(), "lentablecheck", fn);
            B.CreateCondBr(isString, stringBlock, tableCheck);

            B.SetInsertPoint(stringBlock);
            Value* string = toPtr(loadVal(source));
            Value* length = B.CreateLoad(i32Ty, toPtr(addrOf(string, offStringLen)), "length");
            storeNum(destination, B.CreateSIToFP(length, dblTy));
            B.CreateBr(blocks[next]);

            B.SetInsertPoint(tableCheck);
            Value* isTable = B.CreateICmpEQ(loadTT(source), B.getInt32(LUA_TTABLE), "istable");
            BasicBlock* inspectTable = BasicBlock::Create(B.getContext(), "lentable", fn);
            B.CreateCondBr(isTable, inspectTable, fallback);

            B.SetInsertPoint(inspectTable);
            Value* table = toPtr(loadVal(source));
            Value* metatable = B.CreateLoad(ptrTy, toPtr(addrOf(table, offTableMetatable)), "metatable");
            BasicBlock* plainTable = BasicBlock::Create(B.getContext(), "lenplain", fn);
            B.CreateCondBr(B.CreateIsNull(metatable), plainTable, fallback);

            B.SetInsertPoint(plainTable);
            FunctionType* lengthType = FunctionType::get(i32Ty, {ptrTy}, false);
            Value* getLength = contextFunction(nativeContext, offsetof(NativeContext, luaH_getn), "luaH_getn");
            Value* tableLength = B.CreateCall(lengthType, getLength, {table}, "tablelength");
            storeNum(destination, B.CreateSIToFP(tableLength, dblTy));
            B.CreateBr(blocks[next]);
        };

        // Lua closures store immutable upvalue references inline.  An entry
        // can either hold a direct TValue or an UpVal indirection; both are
        // copy-only reads and therefore need no GC barrier.
        auto lowerGetUpval = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* destination = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* ciNow = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "ci");
            Value* function = toPtr(B.CreateLoad(ptrTy, toPtr(addrOf(ciNow, offCiFunc)), "func"));
            Value* closure = toPtr(loadVal(function));
            Value* upref = toPtr(addrOf(closure, offClosureUprefs + uint64_t(LUAU_INSN_B(proto->code[i])) * sizeof(TValue)));
            Value* isIndirect = B.CreateICmpEQ(loadTT(upref), B.getInt32(LUA_TUPVAL), "isupval");

            BasicBlock* indirect = BasicBlock::Create(B.getContext(), "upval", fn);
            BasicBlock* direct = BasicBlock::Create(B.getContext(), "upvaldirect", fn);
            BasicBlock* continueBlock = BasicBlock::Create(B.getContext(), "upvalcontinue", fn);
            B.CreateCondBr(isIndirect, indirect, direct);

            B.SetInsertPoint(indirect);
            Value* upval = toPtr(loadVal(upref));
            Value* value = B.CreateLoad(ptrTy, toPtr(addrOf(upval, offUpValValue)), "upvalvalue");
            copyTV(destination, value);
            B.CreateBr(continueBlock);

            B.SetInsertPoint(direct);
            copyTV(destination, upref);
            B.CreateBr(continueBlock);

            B.SetInsertPoint(continueBlock);
            B.CreateBr(blocks[next]);
        };

        // Upvalue writes need a barrier only for collectable values.  Keep
        // those on the VM path, but write primitive TValue payloads directly.
        auto lowerSetUpval = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* source = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* primitive = B.CreateICmpULT(loadTT(source), B.getInt32(LUA_TSTRING), "primitive");

            BasicBlock* uprefBlock = BasicBlock::Create(B.getContext(), "upref", fn);
            B.CreateCondBr(primitive, uprefBlock, fallback);

            B.SetInsertPoint(uprefBlock);
            Value* ciNow = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "ci");
            Value* function = toPtr(B.CreateLoad(ptrTy, toPtr(addrOf(ciNow, offCiFunc)), "func"));
            Value* closure = toPtr(loadVal(function));
            Value* upref = toPtr(addrOf(closure, offClosureUprefs + uint64_t(LUAU_INSN_B(proto->code[i])) * sizeof(TValue)));
            Value* isIndirect = B.CreateICmpEQ(loadTT(upref), B.getInt32(LUA_TUPVAL), "isupval");

            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
            B.CreateCondBr(isIndirect, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            Value* upval = toPtr(loadVal(upref));
            Value* value = B.CreateLoad(ptrTy, toPtr(addrOf(upval, offUpValValue)), "upvalvalue");
            copyTV(value, source);
            B.CreateBr(blocks[next]);
        };

        // Keep native backedges only while they are free of VM safepoint
        // work.  A pending interrupt or GC step returns to the VM at the
        // current instruction, where its established protocol runs exactly.
        auto lowerBackedgeJump = [&](uint32_t i, int32_t delta, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);

            Value* global = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateGlobal)), "global");
            Value* interrupt = B.CreateLoad(ptrTy, toPtr(addrOf(global, offGlobalCb + offCbInterrupt)), "interrupt");
            Value* noInterrupt = B.CreateIsNull(interrupt);

            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "backedge", fn);
            if (FFlag::LuauBackedgeHeapCheck)
            {
                BasicBlock* gcCheck = BasicBlock::Create(B.getContext(), "gck", fn);
                B.CreateCondBr(noInterrupt, gcCheck, fallback);

                B.SetInsertPoint(gcCheck);
                Value* total = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalTotalBytes)), "total");
                Value* threshold = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalGcThreshold)), "threshold");
                B.CreateCondBr(B.CreateICmpULT(total, threshold, "nogc"), fastBlock, fallback);
            }
            else
                B.CreateCondBr(noInterrupt, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            BasicBlock* target = jumpTo(i, delta, next);
            if (target)
                B.CreateBr(target);
        };

        // A fixed-result return can unwind directly when the caller requests
        // exactly that many values.  Other result shapes retain the VM path,
        // which handles MULTRET and result padding.
        auto lowerReturn = [&](uint32_t i)
        {
            const int resultCount = int(LUAU_INSN_B(proto->code[i])) - 1;
            if (resultCount < 0)
            {
                fb(i);
                return;
            }

            B.SetInsertPoint(blocks[i]);
            Value* ciNow = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "ci");
            Value* requested = B.CreateLoad(i32Ty, toPtr(addrOf(ciNow, offCiNResults)), "nresults");
            Value* supportedResults =
                B.CreateAnd(B.CreateICmpSGE(requested, B.getInt32(0)), B.CreateICmpSLE(requested, B.getInt32(resultCount)), "supportedresults");

            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "returnfast", fn);
            B.CreateCondBr(supportedResults, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            Value* result = B.CreateLoad(ptrTy, toPtr(addrOf(ciNow, offCiFunc)), "result");
            const int firstResult = int(LUAU_INSN_A(proto->code[i]));
            BasicBlock* current = fastBlock;
            for (int j = 0; j < resultCount; ++j)
            {
                BasicBlock* copyBlock = BasicBlock::Create(B.getContext(), "returncopy", fn);
                BasicBlock* nextBlock = BasicBlock::Create(B.getContext(), "returnnext", fn);
                B.SetInsertPoint(current);
                B.CreateCondBr(B.CreateICmpSGT(requested, B.getInt32(j)), copyBlock, nextBlock);
                B.SetInsertPoint(copyBlock);
                copyTV(toPtr(addrOf(result, uint64_t(j) * sizeof(TValue))), regPtrOf(base, firstResult + j));
                B.CreateBr(nextBlock);
                current = nextBlock;
            }

            B.SetInsertPoint(current);

            Value* parent = toPtr(B.CreateSub(B.CreatePtrToInt(ciNow, i64Ty), B.getInt64(sizeof(CallInfo))));
            Value* parentBase = B.CreateLoad(ptrTy, toPtr(addrOf(parent, offCiBase)), "parentbase");
            Value* parentTop = B.CreateLoad(ptrTy, toPtr(addrOf(parent, offCiTop)), "parenttop");
            B.CreateStore(parent, toPtr(addrOf(L, offLStateCi)));
            B.CreateStore(parentBase, toPtr(addrOf(L, offLStateBase)));
            B.CreateStore(parentTop, toPtr(addrOf(L, offLStateTop)));

            Value* flags = B.CreateLoad(i32Ty, toPtr(addrOf(ciNow, offCiFlags)), "flags");
            Value* finalReturn = B.CreateICmpNE(B.CreateAnd(flags, B.getInt32(LUA_CALLINFO_RETURN)), B.getInt32(0), "finalreturn");
            B.CreateRet(B.CreateSelect(finalReturn, B.getInt32(0), B.getInt32(1)));
        };

        // Move fixed parameters past the current top exactly as PREPVARARGS
        // does.  Do this only when the stack already has room; stack growth
        // remains a VM operation.  Resume in the VM afterwards because the
        // generated blocks retain the original base pointer.
        auto lowerPrepVarargs = [&](uint32_t i, uint32_t next)
        {
            const uint32_t numparams = LUAU_INSN_A(proto->code[i]);
            B.SetInsertPoint(blocks[i]);

            Value* top = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateTop)), "top");
            Value* stackLast = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateStackLast)), "stacklast");
            Value* stackSize = B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(p, offProtoMaxStackSize)), "stacksize");
            Value* required = B.CreateAdd(B.CreateZExt(stackSize, i64Ty), B.getInt64(numparams));
            Value* newTop = toPtr(addrOf(top, B.CreateMul(required, B.getInt64(sizeof(TValue)))));
            Value* hasRoom = B.CreateICmpULE(B.CreatePtrToInt(newTop, i64Ty), B.CreatePtrToInt(stackLast, i64Ty), "hasroom");

            BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "varargprep", fn);
            B.CreateCondBr(hasRoom, fastBlock, fallback);

            B.SetInsertPoint(fastBlock);
            for (uint32_t j = 0; j < numparams; ++j)
            {
                copyTV(toPtr(addrOf(top, uint64_t(j) * sizeof(TValue))), regPtrOf(base, int(j)));
                storeNil(regPtrOf(base, int(j)));
            }

            Value* ciNow = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "ci");
            Value* frameTop = toPtr(addrOf(top, B.CreateMul(B.CreateZExt(stackSize, i64Ty), B.getInt64(sizeof(TValue)))));
            B.CreateStore(top, toPtr(addrOf(ciNow, offCiBase)));
            B.CreateStore(frameTop, toPtr(addrOf(ciNow, offCiTop)));
            B.CreateStore(top, toPtr(addrOf(L, offLStateBase)));
            B.CreateStore(frameTop, toPtr(addrOf(L, offLStateTop)));
            fb(next);
        };

        // Fixed-count vararg reads only copy from the prepared storage below
        // the current base.  MULTRET can adjust top and stays in the VM.
        auto lowerGetVarargs = [&](uint32_t i, uint32_t next)
        {
            const int requested = int(LUAU_INSN_B(proto->code[i])) - 1;
            if (requested < 0)
            {
                fb(i);
                return;
            }

            B.SetInsertPoint(blocks[i]);
            Value* ciNow = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "ci");
            Value* function = B.CreateLoad(ptrTy, toPtr(addrOf(ciNow, offCiFunc)), "func");
            Value* numparams = B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(p, offProtoNumParams)), "numparams");
            Value* frameWords =
                B.CreateUDiv(B.CreateSub(B.CreatePtrToInt(base, i64Ty), B.CreatePtrToInt(function, i64Ty)), B.getInt64(sizeof(TValue)));
            Value* varargCount = B.CreateSub(B.CreateSub(frameWords, B.CreateZExt(numparams, i64Ty)), B.getInt64(1), "varargcount");

            BasicBlock* current = B.GetInsertBlock();
            const int destination = int(LUAU_INSN_A(proto->code[i]));
            for (int j = 0; j < requested; ++j)
            {
                BasicBlock* copyBlock = BasicBlock::Create(B.getContext(), "varargcopy", fn);
                BasicBlock* nilBlock = BasicBlock::Create(B.getContext(), "varargnil", fn);
                BasicBlock* joinBlock = BasicBlock::Create(B.getContext(), "varargnext", fn);
                B.SetInsertPoint(current);
                B.CreateCondBr(B.CreateICmpUGT(varargCount, B.getInt64(j)), copyBlock, nilBlock);

                B.SetInsertPoint(copyBlock);
                Value* sourceOffset = B.CreateMul(B.CreateSub(varargCount, B.getInt64(j)), B.getInt64(sizeof(TValue)));
                // base - varargCount + j, expressed to avoid a negative GEP.
                Value* source = toPtr(B.CreateSub(B.CreatePtrToInt(base, i64Ty), sourceOffset));
                copyTV(regPtrOf(base, destination + j), source);
                B.CreateBr(joinBlock);

                B.SetInsertPoint(nilBlock);
                storeNil(regPtrOf(base, destination + j));
                B.CreateBr(joinBlock);

                current = joinBlock;
            }

            B.SetInsertPoint(current);
            B.CreateBr(blocks[next]);
        };

        // Fixed-argument Lua calls can construct the child CallInfo without
        // allocator or metamethod work.  Return to the VM after setup so it
        // enters the child through the normal native-entry protocol.
        auto lowerCall = [&](uint32_t i, uint32_t next)
        {
            const int argumentCount = int(LUAU_INSN_B(proto->code[i])) - 1;
            if (argumentCount < 0)
            {
                fb(i);
                return;
            }

            B.SetInsertPoint(blocks[i]);
            Value* function = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* isFunction = B.CreateICmpEQ(loadTT(function), B.getInt32(LUA_TFUNCTION), "isfunction");
            BasicBlock* functionBlock = BasicBlock::Create(B.getContext(), "callfunction", fn);
            B.CreateCondBr(isFunction, functionBlock, fallback);

            B.SetInsertPoint(functionBlock);
            Value* closure = toPtr(loadVal(function));
            Value* isLua = B.CreateICmpEQ(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(closure, offClosureIsC))), B.getInt8(0), "islua");
            BasicBlock* luaBlock = BasicBlock::Create(B.getContext(), "calllua", fn);
            B.CreateCondBr(isLua, luaBlock, fallback);

            B.SetInsertPoint(luaBlock);
            Value* childProto = B.CreateLoad(ptrTy, toPtr(addrOf(closure, offClosureProto)), "childproto");
            Value* isVararg = B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(childProto, offProtoIsVararg)), "isvararg");
            // Variable-argument children need the VM's exact top management.
            BasicBlock* fixedBlock = BasicBlock::Create(B.getContext(), "callfixed", fn);
            B.CreateCondBr(B.CreateICmpEQ(isVararg, B.getInt8(0)), fixedBlock, fallback);

            B.SetInsertPoint(fixedBlock);
            Value* numparams = B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(childProto, offProtoNumParams)), "numparams");
            Value* childBase = toPtr(addrOf(function, uint64_t(sizeof(TValue))));
            Value* argTop = toPtr(addrOf(childBase, uint64_t(argumentCount) * sizeof(TValue)));
            Value* childStackSize = B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(closure, offClosureStackSize)), "childstacksize");
            Value* childTop = toPtr(addrOf(argTop, B.CreateMul(B.CreateZExt(childStackSize, i64Ty), B.getInt64(sizeof(TValue)))));
            Value* stackLast = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateStackLast)), "stacklast");
            Value* stackHasRoom = B.CreateICmpULE(B.CreatePtrToInt(childTop, i64Ty), B.CreatePtrToInt(stackLast, i64Ty), "stackhasroom");
            BasicBlock* frameBlock = BasicBlock::Create(B.getContext(), "callframe", fn);
            B.CreateCondBr(stackHasRoom, frameBlock, fallback);

            B.SetInsertPoint(frameBlock);
            Value* parentCi = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "parentci");
            Value* childCi = toPtr(addrOf(parentCi, uint64_t(sizeof(CallInfo))));
            Value* endCi = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateEndCi)), "endci");
            BasicBlock* installBlock = BasicBlock::Create(B.getContext(), "callinstall", fn);
            B.CreateCondBr(B.CreateICmpULT(B.CreatePtrToInt(childCi, i64Ty), B.CreatePtrToInt(endCi, i64Ty)), installBlock, fallback);

            B.SetInsertPoint(installBlock);
            BasicBlock* fillCond = BasicBlock::Create(B.getContext(), "callfillcond", fn);
            BasicBlock* fillBody = BasicBlock::Create(B.getContext(), "callfill", fn);
            BasicBlock* afterFill = BasicBlock::Create(B.getContext(), "callfilled", fn);
            B.CreateBr(fillCond);

            B.SetInsertPoint(fillCond);
            llvm::PHINode* fillIndex = B.CreatePHI(i64Ty, 2, "fillindex");
            fillIndex->addIncoming(B.getInt64(argumentCount), installBlock);
            B.CreateCondBr(B.CreateICmpULT(fillIndex, B.CreateZExt(numparams, i64Ty)), fillBody, afterFill);

            B.SetInsertPoint(fillBody);
            storeNil(toPtr(addrOf(childBase, B.CreateMul(fillIndex, B.getInt64(sizeof(TValue))))));
            Value* nextFillIndex = B.CreateAdd(fillIndex, B.getInt64(1));
            B.CreateBr(fillCond);
            fillIndex->addIncoming(nextFillIndex, fillBody);

            B.SetInsertPoint(afterFill);
            Value* childCode = B.CreateLoad(ptrTy, toPtr(addrOf(childProto, offProtoCodeEntry)), "childcode");
            Value* requestedResults = B.getInt32(int(LUAU_INSN_C(proto->code[i])) - 1);
            Value* compiled = B.CreateAnd(
                B.CreateIsNotNull(B.CreateLoad(ptrTy, toPtr(addrOf(childProto, offProtoExecData)))),
                B.CreateICmpNE(B.CreateLoad(i64Ty, toPtr(addrOf(childProto, offProtoExecTarget))), B.getInt64(0)),
                "compiled"
            );
            B.CreateStore(function, toPtr(addrOf(childCi, offCiFunc)));
            B.CreateStore(childBase, toPtr(addrOf(childCi, offCiBase)));
            B.CreateStore(childTop, toPtr(addrOf(childCi, offCiTop)));
            B.CreateStore(childProto, toPtr(addrOf(childCi, offsetof(CallInfo, p))));
            B.CreateStore(childCode, toPtr(addrOf(childCi, offCiSavedPc)));
            B.CreateStore(requestedResults, toPtr(addrOf(childCi, offCiNResults)));
            B.CreateStore(
                B.CreateSelect(compiled, B.getInt32(LUA_CALLINFO_NATIVE), B.getInt32(0), "nativeflags"), toPtr(addrOf(childCi, offCiFlags))
            );

            Value* parentResume = toPtr(addrOf(code, uint64_t(next) * sizeof(Instruction)));
            B.CreateStore(parentResume, toPtr(addrOf(parentCi, offCiSavedPc)));
            B.CreateStore(childCi, toPtr(addrOf(L, offLStateCi)));
            B.CreateStore(childBase, toPtr(addrOf(L, offLStateBase)));
            B.CreateStore(childTop, toPtr(addrOf(L, offLStateTop)));
            B.CreateRet(B.getInt32(1));
        };

        // CALLFB uses the regular call protocol and additionally records its
        // mutable feedback slot.  Run it through the shared runtime helper so
        // stack growth, metamethod calls, C yields, and feedback callbacks keep
        // the VM's exact ordering.  Resume through the VM because any of these
        // operations may replace the stack or the active prototype.
        auto lowerCallFeedback = [&](uint32_t i, uint32_t next)
        {
            const int argumentCount = int(LUAU_INSN_B(proto->code[i])) - 1;

            B.SetInsertPoint(blocks[i]);
            Value* function = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* argTop = argumentCount == LUA_MULTRET ? B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateTop)), "argtop")
                                                         : toPtr(addrOf(function, uint64_t(1 + argumentCount) * sizeof(TValue)));
            Value* parentCi = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "parentci");
            Value* parentResume = toPtr(addrOf(code, uint64_t(next) * sizeof(Instruction)));
            B.CreateStore(parentResume, toPtr(addrOf(parentCi, offCiSavedPc)));

            Value* feedback = toPtr(addrOf(code, uint64_t(i + 1) * sizeof(Instruction)));
            FunctionType* callType = FunctionType::get(ptrTy, {ptrTy, ptrTy, ptrTy, i32Ty, ptrTy}, false);
            Value* call = contextFunction(nativeContext, offsetof(NativeContext, callFeedbackFallback), "callfeedback");
            Value* result = B.CreateCall(callType, call, {L, function, argTop, B.getInt32(int(LUAU_INSN_C(proto->code[i])) - 1), feedback});
            Value* yielded = B.CreateICmpEQ(B.CreatePtrToInt(result, i64Ty), B.getInt64(CALL_FALLBACK_YIELD), "yielded");

            BasicBlock* yieldBlock = BasicBlock::Create(B.getContext(), "callfbyield", fn);
            BasicBlock* resumeBlock = BasicBlock::Create(B.getContext(), "callfbresume", fn);
            B.CreateCondBr(yielded, yieldBlock, resumeBlock);

            B.SetInsertPoint(yieldBlock);
            B.CreateRet(B.getInt32(0));

            B.SetInsertPoint(resumeBlock);
            B.CreateRet(B.getInt32(1));
        };

        // luaF_close only has work when the newest open upvalue belongs to
        // this register range.  Preserve the VM call for that case.
        auto lowerCloseUpvals = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* level = regPtrOf(base, int(LUAU_INSN_A(proto->code[i])));
            Value* open = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateOpenUpval)), "openupval");
            BasicBlock* checkBlock = BasicBlock::Create(B.getContext(), "closecheck", fn);
            BasicBlock* continueBlock = BasicBlock::Create(B.getContext(), "closenone", fn);
            B.CreateCondBr(B.CreateIsNull(open), continueBlock, checkBlock);

            B.SetInsertPoint(checkBlock);
            Value* openValue = B.CreateLoad(ptrTy, toPtr(addrOf(open, offUpValValue)), "openvalue");
            B.CreateCondBr(B.CreateICmpULT(B.CreatePtrToInt(openValue, i64Ty), B.CreatePtrToInt(level, i64Ty)), continueBlock, fallback);

            B.SetInsertPoint(continueBlock);
            B.CreateBr(blocks[next]);
        };

        // Coverage counters live in the original bytecode instruction.  The
        // E field is a saturated 23-bit hit count, so increment bit 8 only
        // while the field remains below the VM's maximum.
        auto lowerCoverage = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            Value* instruction = toPtr(addrOf(code, uint64_t(i) * sizeof(Instruction)));
            Value* current = B.CreateLoad(i32Ty, instruction, "coverage");
            Value* hits = B.CreateAShr(current, B.getInt32(8), "hits");
            Value* increment = B.CreateAdd(current, B.getInt32(1 << 8));
            Value* updated = B.CreateSelect(B.CreateICmpSLT(hits, B.getInt32((1 << 23) - 1)), increment, current);
            B.CreateStore(updated, instruction);
            B.CreateBr(blocks[next]);
        };

        // FORGPREP_INEXT only needs setup for the safe-environment ipairs
        // form.  The general iterator form can invoke __iter/__call and must
        // remain in the VM.
        auto lowerForGPrepInext = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            const uint32_t a = LUAU_INSN_A(proto->code[i]);
            Value* ra = regPtrOf(base, int(a));
            Value* table = toPtr(addrOf(ra, sizeof(TValue)));
            Value* index = toPtr(addrOf(ra, 2 * sizeof(TValue)));
            Value* function = toPtr(addrOf(ci, offCiFunc));
            Value* closure = toPtr(loadVal(function));
            Value* env = B.CreateLoad(ptrTy, toPtr(addrOf(closure, offClosureEnv)), "env");

            Value* safeEnv = B.CreateAnd(
                B.CreateIsNotNull(env), B.CreateICmpNE(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(env, offTableSafeEnv))), B.getInt8(0)), "safeenv"
            );
            Value* eligible = B.CreateAnd(
                safeEnv,
                B.CreateAnd(
                    B.CreateICmpEQ(loadTT(table), B.getInt32(LUA_TTABLE)),
                    B.CreateAnd(B.CreateICmpEQ(loadTT(index), B.getInt32(LUA_TNUMBER)), B.CreateFCmpOEQ(loadNum(index), ConstantFP::get(dblTy, 0.0)))
                ),
                "inext"
            );

            BasicBlock* fast = BasicBlock::Create(B.getContext(), "inext", fn);
            B.CreateCondBr(eligible, fast, fallback);

            B.SetInsertPoint(fast);
            storeNil(ra);
            B.CreateStore(B.getInt64(0), index);
            B.CreateStore(B.getInt32(LU_TAG_ITERATOR), toPtr(addrOf(index, offsetof(TValue, extra))));
            B.CreateStore(B.getInt32(LUA_TLIGHTUSERDATA), toPtr(addrOf(index, offsetof(TValue, tt))));

            BasicBlock* target = jumpTo(i, LUAU_INSN_D(proto->code[i]), next);
            if (target)
                B.CreateBr(target);
        };

        // FORGPREP_NEXT has the same non-allocating setup as inext but
        // requires a nil iteration key instead of numeric zero.
        auto lowerForGPrepNext = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            const uint32_t a = LUAU_INSN_A(proto->code[i]);
            Value* ra = regPtrOf(base, int(a));
            Value* table = toPtr(addrOf(ra, sizeof(TValue)));
            Value* index = toPtr(addrOf(ra, 2 * sizeof(TValue)));
            Value* function = toPtr(addrOf(ci, offCiFunc));
            Value* closure = toPtr(loadVal(function));
            Value* env = B.CreateLoad(ptrTy, toPtr(addrOf(closure, offClosureEnv)), "env");
            Value* safeEnv = B.CreateAnd(
                B.CreateIsNotNull(env), B.CreateICmpNE(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(env, offTableSafeEnv))), B.getInt8(0)), "safeenv"
            );
            Value* eligible = B.CreateAnd(
                safeEnv,
                B.CreateAnd(B.CreateICmpEQ(loadTT(table), B.getInt32(LUA_TTABLE)), B.CreateICmpEQ(loadTT(index), B.getInt32(LUA_TNIL))),
                "next"
            );

            BasicBlock* fast = BasicBlock::Create(B.getContext(), "nextprep", fn);
            B.CreateCondBr(eligible, fast, fallback);

            B.SetInsertPoint(fast);
            storeNil(ra);
            B.CreateStore(B.getInt64(0), index);
            B.CreateStore(B.getInt32(LU_TAG_ITERATOR), toPtr(addrOf(index, offsetof(TValue, extra))));
            B.CreateStore(B.getInt32(LUA_TLIGHTUSERDATA), toPtr(addrOf(index, offsetof(TValue, tt))));

            BasicBlock* target = jumpTo(i, LUAU_INSN_D(proto->code[i]), next);
            if (target)
                B.CreateBr(target);
        };

        // A plain table in FORGPREP receives the same builtin iterator state
        // as pairs.  Tables with metatables can define __iter or __call and
        // therefore resume in the VM.
        auto lowerForGPrepTable = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            const uint32_t a = LUAU_INSN_A(proto->code[i]);
            Value* ra = regPtrOf(base, int(a));
            Value* state = toPtr(addrOf(ra, sizeof(TValue)));
            Value* index = toPtr(addrOf(ra, 2 * sizeof(TValue)));
            Value* plainTable = B.CreateICmpEQ(loadTT(ra), B.getInt32(LUA_TTABLE), "table");
            BasicBlock* inspect = BasicBlock::Create(B.getContext(), "forginspect", fn);
            B.CreateCondBr(plainTable, inspect, fallback);

            B.SetInsertPoint(inspect);
            Value* h = toPtr(loadVal(ra));
            Value* metatable = B.CreateLoad(ptrTy, toPtr(addrOf(h, offTableMetatable)), "metatable");
            BasicBlock* fast = BasicBlock::Create(B.getContext(), "forgtable", fn);
            B.CreateCondBr(B.CreateIsNull(metatable), fast, fallback);

            B.SetInsertPoint(fast);
            copyTV(state, ra);
            storeNil(ra);
            B.CreateStore(B.getInt64(0), index);
            B.CreateStore(B.getInt32(LU_TAG_ITERATOR), toPtr(addrOf(index, offsetof(TValue, extra))));
            B.CreateStore(B.getInt32(LUA_TLIGHTUSERDATA), toPtr(addrOf(index, offsetof(TValue, tt))));

            BasicBlock* target = jumpTo(i, LUAU_INSN_D(proto->code[i]), next);
            if (target)
                B.CreateBr(target);
        };

        // Native ipairs iteration covers the array part only.  A hash-table
        // continuation, a sparse array hole, a callback, or a GC/interrupt
        // safepoint resumes at this instruction in the VM.
        auto lowerForGLoopIpairs = [&](uint32_t i, uint32_t next)
        {
            const int32_t aux = int32_t(auxOf(proto, i));
            if (aux >= 0)
            {
                B.SetInsertPoint(blocks[i]);
                fb(i);
                return;
            }

            B.SetInsertPoint(blocks[i]);
            const uint32_t a = LUAU_INSN_A(proto->code[i]);
            Value* ra = regPtrOf(base, int(a));
            Value* table = toPtr(addrOf(ra, sizeof(TValue)));
            Value* iterator = toPtr(addrOf(ra, 2 * sizeof(TValue)));

            Value* global = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateGlobal)), "global");
            Value* interrupt = B.CreateLoad(ptrTy, toPtr(addrOf(global, offGlobalCb + offCbInterrupt)), "interrupt");
            Value* safePoint = B.CreateIsNull(interrupt, "safept");
            if (FFlag::LuauBackedgeHeapCheck)
            {
                Value* total = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalTotalBytes)), "total");
                Value* threshold = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalGcThreshold)), "threshold");
                safePoint = B.CreateAnd(safePoint, B.CreateICmpSLT(total, threshold), "safegc");
            }
            Value* valid = B.CreateAnd(
                safePoint,
                B.CreateAnd(
                    B.CreateICmpEQ(loadTT(ra), B.getInt32(LUA_TNIL)),
                    B.CreateAnd(
                        B.CreateICmpEQ(loadTT(table), B.getInt32(LUA_TTABLE)),
                        B.CreateAnd(
                            B.CreateICmpEQ(loadTT(iterator), B.getInt32(LUA_TLIGHTUSERDATA)),
                            B.CreateICmpEQ(B.CreateLoad(i32Ty, toPtr(addrOf(iterator, offsetof(TValue, extra)))), B.getInt32(LU_TAG_ITERATOR))
                        )
                    )
                ),
                "ipairs"
            );
            BasicBlock* scan = BasicBlock::Create(B.getContext(), "ipairs_scan", fn);
            B.CreateCondBr(valid, scan, fallback);

            B.SetInsertPoint(scan);
            Value* h = toPtr(loadVal(table));
            Value* index64 = B.CreateLoad(i64Ty, iterator, "index");
            Value* indexInRange = B.CreateICmpULE(index64, B.getInt64(INT_MAX), "indexrange");
            BasicBlock* bounds = BasicBlock::Create(B.getContext(), "ipairs_bounds", fn);
            B.CreateCondBr(indexInRange, bounds, fallback);

            B.SetInsertPoint(bounds);
            Value* size = B.CreateLoad(i32Ty, toPtr(addrOf(h, offTableSizeArray)), "sizearray");
            Value* beforeEnd = B.CreateICmpULT(index64, B.CreateZExt(size, i64Ty), "arraybounds");
            BasicBlock* elementBlock = BasicBlock::Create(B.getContext(), "ipairs_elem", fn);
            B.CreateCondBr(beforeEnd, elementBlock, blocks[next]);

            B.SetInsertPoint(elementBlock);
            Value* array = B.CreateLoad(ptrTy, toPtr(addrOf(h, offTableArray)), "array");
            Value* element = toPtr(addrOf(array, B.CreateMul(index64, B.getInt64(sizeof(TValue)))));
            Value* present = B.CreateICmpNE(loadTT(element), B.getInt32(LUA_TNIL), "present");
            BasicBlock* yield = BasicBlock::Create(B.getContext(), "ipairs_yield", fn);
            B.CreateCondBr(present, yield, blocks[next]);

            B.SetInsertPoint(yield);
            Value* nextIndex = B.CreateAdd(index64, B.getInt64(1));
            B.CreateStore(nextIndex, iterator);
            B.CreateStore(B.getInt32(LU_TAG_ITERATOR), toPtr(addrOf(iterator, offsetof(TValue, extra))));
            B.CreateStore(B.getInt32(LUA_TLIGHTUSERDATA), toPtr(addrOf(iterator, offsetof(TValue, tt))));
            Value* key = toPtr(addrOf(ra, 3 * sizeof(TValue)));
            storeNum(key, B.CreateUIToFP(nextIndex, dblTy));
            copyTV(toPtr(addrOf(ra, 4 * sizeof(TValue))), element);

            BasicBlock* target = jumpTo(i, LUAU_INSN_D(proto->code[i]), next);
            if (target)
                B.CreateBr(target);
        };

        // Native pairs traversal walks the array portion and skips holes.  It
        // hands the first hash slot back to the VM, which retains the complete
        // hash iteration and error/callback behavior.
        auto lowerForGLoopPairs = [&](uint32_t i, uint32_t next)
        {
            const uint32_t aux = auxOf(proto, i);
            if (int32_t(aux) < 0)
            {
                B.SetInsertPoint(blocks[i]);
                fb(i);
                return;
            }

            B.SetInsertPoint(blocks[i]);
            const uint32_t a = LUAU_INSN_A(proto->code[i]);
            Value* ra = regPtrOf(base, int(a));
            Value* table = toPtr(addrOf(ra, sizeof(TValue)));
            Value* iterator = toPtr(addrOf(ra, 2 * sizeof(TValue)));
            Value* global = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateGlobal)), "global");
            Value* interrupt = B.CreateLoad(ptrTy, toPtr(addrOf(global, offGlobalCb + offCbInterrupt)), "interrupt");
            Value* safePoint = B.CreateIsNull(interrupt, "safept");
            if (FFlag::LuauBackedgeHeapCheck)
            {
                Value* total = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalTotalBytes)), "total");
                Value* threshold = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalGcThreshold)), "threshold");
                safePoint = B.CreateAnd(safePoint, B.CreateICmpSLT(total, threshold), "safegc");
            }
            Value* valid = B.CreateAnd(
                safePoint,
                B.CreateAnd(
                    B.CreateICmpEQ(loadTT(ra), B.getInt32(LUA_TNIL)),
                    B.CreateAnd(
                        B.CreateICmpEQ(loadTT(table), B.getInt32(LUA_TTABLE)),
                        B.CreateAnd(
                            B.CreateICmpEQ(loadTT(iterator), B.getInt32(LUA_TLIGHTUSERDATA)),
                            B.CreateICmpEQ(B.CreateLoad(i32Ty, toPtr(addrOf(iterator, offsetof(TValue, extra)))), B.getInt32(LU_TAG_ITERATOR))
                        )
                    )
                ),
                "pairs"
            );
            BasicBlock* start = BasicBlock::Create(B.getContext(), "pairs_start", fn);
            B.CreateCondBr(valid, start, fallback);

            B.SetInsertPoint(start);
            Value* h = toPtr(loadVal(table));
            Value* initial = B.CreateLoad(i64Ty, iterator, "index");
            Value* inRange = B.CreateICmpULE(initial, B.getInt64(INT_MAX), "indexrange");
            BasicBlock* scan = BasicBlock::Create(B.getContext(), "pairs_scan", fn);
            B.CreateCondBr(inRange, scan, fallback);

            B.SetInsertPoint(scan);
            llvm::PHINode* index = B.CreatePHI(i64Ty, 2, "pairs_index");
            index->addIncoming(initial, start);
            Value* size = B.CreateLoad(i32Ty, toPtr(addrOf(h, offTableSizeArray)), "sizearray");
            Value* beforeEnd = B.CreateICmpULT(index, B.CreateZExt(size, i64Ty), "arraybounds");
            BasicBlock* elementBlock = BasicBlock::Create(B.getContext(), "pairs_elem", fn);
            BasicBlock* hashFallback = BasicBlock::Create(B.getContext(), "pairs_hash", fn);
            B.CreateCondBr(beforeEnd, elementBlock, hashFallback);

            B.SetInsertPoint(hashFallback);
            B.CreateStore(index, iterator);
            B.CreateStore(B.getInt32(LU_TAG_ITERATOR), toPtr(addrOf(iterator, offsetof(TValue, extra))));
            B.CreateStore(B.getInt32(LUA_TLIGHTUSERDATA), toPtr(addrOf(iterator, offsetof(TValue, tt))));
            fb(i);

            B.SetInsertPoint(elementBlock);
            Value* array = B.CreateLoad(ptrTy, toPtr(addrOf(h, offTableArray)), "array");
            Value* element = toPtr(addrOf(array, B.CreateMul(index, B.getInt64(sizeof(TValue)))));
            Value* present = B.CreateICmpNE(loadTT(element), B.getInt32(LUA_TNIL), "present");
            BasicBlock* yield = BasicBlock::Create(B.getContext(), "pairs_yield", fn);
            BasicBlock* skip = BasicBlock::Create(B.getContext(), "pairs_skip", fn);
            B.CreateCondBr(present, yield, skip);

            B.SetInsertPoint(skip);
            Value* skipped = B.CreateAdd(index, B.getInt64(1));
            B.CreateStore(skipped, iterator);
            B.CreateBr(scan);
            index->addIncoming(skipped, skip);

            B.SetInsertPoint(yield);
            Value* nextIndex = B.CreateAdd(index, B.getInt64(1));
            B.CreateStore(nextIndex, iterator);
            B.CreateStore(B.getInt32(LU_TAG_ITERATOR), toPtr(addrOf(iterator, offsetof(TValue, extra))));
            B.CreateStore(B.getInt32(LUA_TLIGHTUSERDATA), toPtr(addrOf(iterator, offsetof(TValue, tt))));
            Value* key = toPtr(addrOf(ra, 3 * sizeof(TValue)));
            storeNum(key, B.CreateUIToFP(nextIndex, dblTy));
            copyTV(toPtr(addrOf(ra, 4 * sizeof(TValue))), element);
            for (uint32_t result = 2; result < aux; ++result)
                storeNil(toPtr(addrOf(ra, uint64_t(3 + result) * sizeof(TValue))));

            BasicBlock* target = jumpTo(i, LUAU_INSN_D(proto->code[i]), next);
            if (target)
                B.CreateBr(target);
        };

        auto saveResumePc = [&](uint32_t next)
        {
            Value* currentCi = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "currentci");
            Value* resume = toPtr(addrOf(code, uint64_t(next) * sizeof(Instruction)));
            B.CreateStore(resume, toPtr(addrOf(currentCi, offCiSavedPc)));
        };

        // Allocation helpers use the same NativeContext function table as the
        // assembly backend.  Resume in the VM after the operation so any
        // stack or CallInfo relocation performed by callbacks cannot leave
        // stale LLVM SSA pointers live.
        auto finishAllocatingOpcode = [&]()
        {
            Value* global = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateGlobal)), "global");
            Value* total = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalTotalBytes)), "total");
            Value* threshold = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalGcThreshold)), "threshold");
            BasicBlock* collect = BasicBlock::Create(B.getContext(), "allocgc", fn);
            BasicBlock* done = BasicBlock::Create(B.getContext(), "allocdone", fn);
            B.CreateCondBr(B.CreateICmpSGE(total, threshold), collect, done);

            B.SetInsertPoint(collect);
            FunctionType* stepType = FunctionType::get(i64Ty, {ptrTy, i1Ty}, false);
            Value* step = contextFunction(nativeContext, offsetof(NativeContext, luaC_step), "luaC_step");
            B.CreateCall(stepType, step, {L, B.getInt1(true)});
            B.CreateBr(done);

            B.SetInsertPoint(done);
            B.CreateRet(B.getInt32(1));
        };

        auto lowerNewTable = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            saveResumePc(next);
            const uint32_t insn = proto->code[i];
            const uint32_t encodedHashSize = LUAU_INSN_B(insn);
            if (encodedHashSize > 31)
            {
                fb(i);
                return;
            }
            const int hashSize = encodedHashSize == 0 ? 0 : int(1u << (encodedHashSize - 1));
            FunctionType* newType = FunctionType::get(ptrTy, {ptrTy, i32Ty, i32Ty}, false);
            Value* create = contextFunction(nativeContext, offsetof(NativeContext, luaH_new), "luaH_new");
            Value* table = B.CreateCall(newType, create, {L, B.getInt32(int(auxOf(proto, i))), B.getInt32(hashSize)}, "newtable");
            Value* destination = regPtrOf(base, int(LUAU_INSN_A(insn)));
            B.CreateStore(B.CreatePtrToInt(table, i64Ty), destination);
            B.CreateStore(B.getInt32(LUA_TTABLE), toPtr(addrOf(destination, offsetof(TValue, tt))));
            finishAllocatingOpcode();
        };

        auto lowerDupTable = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            saveResumePc(next);
            const uint32_t insn = proto->code[i];
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            Value* source = toPtr(addrOf(constants, uint64_t(LUAU_INSN_D(insn)) * sizeof(TValue)));
            FunctionType* cloneType = FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
            Value* clone = contextFunction(nativeContext, offsetof(NativeContext, luaH_clone), "luaH_clone");
            Value* table = B.CreateCall(cloneType, clone, {L, toPtr(loadVal(source))}, "duptable");
            Value* destination = regPtrOf(base, int(LUAU_INSN_A(insn)));
            B.CreateStore(B.CreatePtrToInt(table, i64Ty), destination);
            B.CreateStore(B.getInt32(LUA_TTABLE), toPtr(addrOf(destination, offsetof(TValue, tt))));
            finishAllocatingOpcode();
        };

        auto lowerConcat = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            saveResumePc(next);
            const uint32_t insn = proto->code[i];
            const int first = LUAU_INSN_B(insn);
            const int last = LUAU_INSN_C(insn);
            FunctionType* concatType = FunctionType::get(B.getVoidTy(), {ptrTy, i32Ty, i32Ty}, false);
            Value* concat = contextFunction(nativeContext, offsetof(NativeContext, luaV_concat), "luaV_concat");
            B.CreateCall(concatType, concat, {L, B.getInt32(last - first + 1), B.getInt32(last)});

            Value* refreshedBase = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateBase)), "base_after_concat");
            copyTV(regPtrOf(refreshedBase, int(LUAU_INSN_A(insn))), regPtrOf(refreshedBase, first));
            finishAllocatingOpcode();
        };

        auto lowerNewClosure = [&](uint32_t i, uint32_t next)
        {
            B.SetInsertPoint(blocks[i]);
            saveResumePc(next);
            const uint32_t insn = proto->code[i];
            const uint32_t childIndex = uint32_t(LUAU_INSN_D(insn));
            Value* children = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoP)), "children");
            Value* child = B.CreateLoad(ptrTy, toPtr(addrOf(children, uint64_t(childIndex) * sizeof(Proto*))), "child");
            Value* nups = B.CreateZExt(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(child, offProtoNups))), i32Ty, "nups");
            Value* function = toPtr(addrOf(ci, offCiFunc));
            Value* currentClosure = toPtr(loadVal(function));
            Value* env = B.CreateLoad(ptrTy, toPtr(addrOf(currentClosure, offClosureEnv)), "env");

            FunctionType* createType = FunctionType::get(ptrTy, {ptrTy, i32Ty, ptrTy, ptrTy}, false);
            Value* create = contextFunction(nativeContext, offsetof(NativeContext, luaF_newLclosure), "luaF_newLclosure");
            Value* closure = B.CreateCall(createType, create, {L, nups, env, child}, "newclosure");
            Value* destination = regPtrOf(base, int(LUAU_INSN_A(insn)));
            B.CreateStore(B.CreatePtrToInt(closure, i64Ty), destination);
            B.CreateStore(B.getInt32(LUA_TFUNCTION), toPtr(addrOf(destination, offsetof(TValue, tt))));

            for (uint32_t upvalue = 0; upvalue < uint32_t(proto->p[childIndex]->nups); ++upvalue)
            {
                const Instruction capture = proto->code[i + 1 + upvalue];
                CODEGEN_ASSERT(LUAU_INSN_OP(capture) == LOP_CAPTURE);
                Value* target = toPtr(addrOf(closure, offClosureUprefs + uint64_t(upvalue) * sizeof(TValue)));
                switch (LUAU_INSN_A(capture))
                {
                case LCT_VAL:
                    copyTV(target, regPtrOf(base, int(LUAU_INSN_B(capture))));
                    break;
                case LCT_REF:
                {
                    FunctionType* findType = FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
                    Value* find = contextFunction(nativeContext, offsetof(NativeContext, luaF_findupval), "luaF_findupval");
                    Value* upval = B.CreateCall(findType, find, {L, regPtrOf(base, int(LUAU_INSN_B(capture)))}, "upval");
                    B.CreateStore(B.CreatePtrToInt(upval, i64Ty), target);
                    B.CreateStore(B.getInt32(LUA_TUPVAL), toPtr(addrOf(target, offsetof(TValue, tt))));
                    break;
                }
                case LCT_UPVAL:
                {
                    Value* source = toPtr(addrOf(currentClosure, offClosureUprefs + uint64_t(LUAU_INSN_B(capture)) * sizeof(TValue)));
                    copyTV(target, source);
                    break;
                }
                default:
                    CODEGEN_ASSERT(false);
                }
            }

            finishAllocatingOpcode();
        };

        auto lowerDupClosure = [&](uint32_t i)
        {
            B.SetInsertPoint(blocks[i]);
            Value* pc = toPtr(addrOf(code, uint64_t(i) * sizeof(Instruction)));
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            FunctionType* helperType = FunctionType::get(ptrTy, {ptrTy, ptrTy, ptrTy, ptrTy}, false);
            Value* helper = contextFunction(nativeContext, offsetof(NativeContext, executeDUPCLOSURE), "executeDUPCLOSURE");
            Value* resume = B.CreateCall(helperType, helper, {L, pc, base, constants}, "dupclosure_resume");
            Value* currentCi = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "currentci");
            B.CreateStore(resume, toPtr(addrOf(currentCi, offCiSavedPc)));
            B.CreateRet(B.getInt32(1));
        };

        // Class construction mutates shared runtime metadata and can allocate
        // while inheriting or adding members.  Keep that logic in the VM
        // helper, but enter it directly from compiled LLVM code and resume at
        // the following bytecode instruction.
        auto lowerClassOpcode = [&](uint32_t i, uint64_t helperOffset, const char* helperName)
        {
            B.SetInsertPoint(blocks[i]);
            Value* pc = toPtr(addrOf(code, uint64_t(i) * sizeof(Instruction)));
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            FunctionType* helperType = FunctionType::get(ptrTy, {ptrTy, ptrTy, ptrTy, ptrTy}, false);
            Value* helper = contextFunction(nativeContext, helperOffset, helperName);
            Value* resume = B.CreateCall(helperType, helper, {L, pc, base, constants}, "class_resume");
            Value* currentCi = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "currentci");
            B.CreateStore(resume, toPtr(addrOf(currentCi, offCiSavedPc)));
            B.CreateRet(B.getInt32(1));
        };

        // Direct-userdata callbacks temporarily install C CallInfo frames and
        // may reallocate the stack.  Execute the exact callback protocol in a
        // runtime helper, then hand the returned resume PC to the VM.  A null
        // resume denotes a yielded __namecall.
        auto lowerUserdataOpcode = [&](uint32_t i, uint64_t helperOffset, const char* helperName, bool canYield)
        {
            B.SetInsertPoint(blocks[i]);
            Value* pc = toPtr(addrOf(code, uint64_t(i) * sizeof(Instruction)));
            Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
            FunctionType* helperType = FunctionType::get(ptrTy, {ptrTy, ptrTy, ptrTy, ptrTy}, false);
            Value* helper = contextFunction(nativeContext, helperOffset, helperName);
            Value* resume = B.CreateCall(helperType, helper, {L, pc, base, constants}, "userdata_resume");

            BasicBlock* resumeBlock = BasicBlock::Create(B.getContext(), "userdata_resume", fn);
            if (canYield)
            {
                BasicBlock* yieldBlock = BasicBlock::Create(B.getContext(), "userdata_yield", fn);
                B.CreateCondBr(B.CreateIsNull(resume), yieldBlock, resumeBlock);
                B.SetInsertPoint(yieldBlock);
                B.CreateRet(B.getInt32(0));
            }
            else
                B.CreateBr(resumeBlock);

            B.SetInsertPoint(resumeBlock);
            Value* currentCi = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "currentci");
            B.CreateStore(resume, toPtr(addrOf(currentCi, offCiSavedPc)));
            B.CreateRet(B.getInt32(1));
        };

        // operand decoding walk
        uint32_t i = 0;
        while (i < sizecode)
        {
            const uint32_t insn = proto->code[i];
            const uint8_t op = LUAU_INSN_OP(insn);
            const uint32_t words = instructionWords(proto, insn);
            const uint32_t next = i + words;

            B.SetInsertPoint(blocks[i]);
            B.CreateStore(B.getInt64(i), fbSlot);

            switch (op)
            {
            case LOP_NOP:
            {
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_BREAK:
            case LOP_CAPTURE:
                // BREAK needs the debugger hook and CAPTURE is only valid as
                // payload consumed by NEWCLOSURE/DUPCLOSURE.
                fb(i);
                break;

            case LOP_NATIVECALL:
                // NATIVECALL is an out-of-proto entry trampoline.  A saved PC
                // must never address it; stop defensively instead of recursing.
                B.CreateRet(B.getInt32(0));
                break;

            case LOP_LOADNIL:
            {
                storeNil(regPtrOf(base, int(LUAU_INSN_A(insn))));
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_LOADB:
            {
                storeBool(regPtrOf(base, int(LUAU_INSN_A(insn))), B.getInt1(LUAU_INSN_B(insn) != 0));
                const int32_t delta = LUAU_INSN_C(insn);
                if (delta == 0)
                    B.CreateBr(blocks[next]);
                else
                {
                    BasicBlock* target = jumpTo(i, delta, next);
                    if (target)
                        B.CreateBr(target);
                }
                break;
            }

            case LOP_LOADN:
            {
                storeNum(regPtrOf(base, int(LUAU_INSN_A(insn))), ConstantFP::get(dblTy, double(LUAU_INSN_D(insn))));
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_LOADK:
            case LOP_LOADKX:
            {
                const uint32_t constantIndex = op == LOP_LOADK ? uint32_t(LUAU_INSN_D(insn)) : auxOf(proto, i);
                Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
                Value* constant = toPtr(addrOf(constants, uint64_t(constantIndex) * sizeof(TValue)));
                copyTV(regPtrOf(base, int(LUAU_INSN_A(insn))), constant);
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_GETIMPORT:
            {
                Value* destination = regPtrOf(base, int(LUAU_INSN_A(insn)));
                Value* constants = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
                Value* imported = toPtr(addrOf(constants, uint64_t(LUAU_INSN_D(insn)) * sizeof(TValue)));
                Value* function = toPtr(addrOf(ci, offCiFunc));
                Value* closure = toPtr(loadVal(function));
                Value* env = B.CreateLoad(ptrTy, toPtr(addrOf(closure, offClosureEnv)), "env");
                Value* safe = B.CreateAnd(
                    B.CreateIsNotNull(env), B.CreateICmpNE(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(env, offTableSafeEnv))), B.getInt8(0)), "safeenv"
                );
                Value* cached = B.CreateICmpNE(loadTT(imported), B.getInt32(LUA_TNIL), "cachedimport");
                BasicBlock* fast = BasicBlock::Create(B.getContext(), "import", fn);
                B.CreateCondBr(B.CreateAnd(safe, cached), fast, fallback);
                B.SetInsertPoint(fast);
                copyTV(destination, imported);
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_MOVE:
            {
                copyTV(regPtrOf(base, int(LUAU_INSN_A(insn))), regPtrOf(base, int(LUAU_INSN_B(insn))));
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_NOT:
            {
                storeBool(regPtrOf(base, int(LUAU_INSN_A(insn))), isFalse(regPtrOf(base, int(LUAU_INSN_B(insn)))));
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_AND:
            case LOP_ANDK:
            {
                // rA = isfalse(rB) ? rB : (rC or kC)
                Value* rb = regPtrOf(base, int(LUAU_INSN_B(insn)));
                Value* rc;
                if (op == LOP_ANDK)
                {
                    Value* k = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
                    rc = toPtr(addrOf(k, uint64_t(LUAU_INSN_C(insn)) * sizeof(TValue)));
                }
                else
                {
                    rc = regPtrOf(base, int(LUAU_INSN_C(insn)));
                }
                Value* src = B.CreateSelect(isFalse(rb), rb, rc, "src");
                copyTV(regPtrOf(base, int(LUAU_INSN_A(insn))), src);
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_OR:
            case LOP_ORK:
            {
                // rA = isfalse(rB) ? (rC or kC) : rB
                Value* rb = regPtrOf(base, int(LUAU_INSN_B(insn)));
                Value* rc;
                if (op == LOP_ORK)
                {
                    Value* k = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoK)), "k");
                    rc = toPtr(addrOf(k, uint64_t(LUAU_INSN_C(insn)) * sizeof(TValue)));
                }
                else
                {
                    rc = regPtrOf(base, int(LUAU_INSN_C(insn)));
                }
                Value* src = B.CreateSelect(isFalse(rb), rc, rb, "src");
                copyTV(regPtrOf(base, int(LUAU_INSN_A(insn))), src);
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_GETGLOBAL:
                lowerGetGlobal(i, next);
                break;

            case LOP_GETTABLEKS:
                lowerGetTableKs(i, next);
                break;

            case LOP_SETGLOBAL:
                lowerSetGlobal(i, next);
                break;

            case LOP_SETTABLEKS:
                lowerSetTableKs(i, next);
                break;

            case LOP_NAMECALL:
                lowerNameCall(i, next);
                break;

            case LOP_NEWTABLE:
                lowerNewTable(i, next);
                break;

            case LOP_DUPTABLE:
                lowerDupTable(i, next);
                break;

            case LOP_CONCAT:
                lowerConcat(i, next);
                break;

            case LOP_DUPCLOSURE:
                lowerDupClosure(i);
                break;

            case LOP_NEWCLOSURE:
                lowerNewClosure(i, next);
                break;

            case LOP_GETTABLEN:
                lowerGetTableN(i, next);
                break;

            case LOP_GETTABLE:
                lowerGetTable(i, next);
                break;

            case LOP_SETTABLEN:
                lowerSetTableN(i, next);
                break;

            case LOP_SETTABLE:
                lowerSetTable(i, next);
                break;

            case LOP_SETLIST:
                lowerSetList(i, next);
                break;

            case LOP_LENGTH:
                lowerLength(i, next);
                break;

            case LOP_GETUPVAL:
                lowerGetUpval(i, next);
                break;

            case LOP_SETUPVAL:
                lowerSetUpval(i, next);
                break;

            case LOP_JUMP:
            {
                const int32_t delta = LUAU_INSN_D(insn);
                if (delta == 0)
                    B.CreateBr(blocks[next]);
                else
                {
                    BasicBlock* target = jumpTo(i, delta, next);
                    if (target)
                        B.CreateBr(target);
                }
                break;
            }

            case LOP_JUMPBACK:
                lowerBackedgeJump(i, LUAU_INSN_D(insn), next);
                break;

            case LOP_JUMPX:
                lowerBackedgeJump(i, LUAU_INSN_E(insn), next);
                break;

            case LOP_RETURN:
                lowerReturn(i);
                break;

            case LOP_PREPVARARGS:
                lowerPrepVarargs(i, next);
                break;

            case LOP_GETVARARGS:
                lowerGetVarargs(i, next);
                break;

            case LOP_CALL:
                lowerCall(i, next);
                break;

            case LOP_CALLFB:
                lowerCallFeedback(i, next);
                break;

            case LOP_NEWCLASSMEMBER:
                lowerClassOpcode(i, offsetof(NativeContext, executeNEWCLASSMEMBER), "executeNEWCLASSMEMBER");
                break;

            case LOP_NEWCLASS:
                lowerClassOpcode(i, offsetof(NativeContext, executeNEWCLASS), "executeNEWCLASS");
                break;

            case LOP_GETUDATAKS:
                lowerUserdataOpcode(i, offsetof(NativeContext, executeGETUDATAKS), "executeGETUDATAKS", false);
                break;

            case LOP_SETUDATAKS:
                lowerUserdataOpcode(i, offsetof(NativeContext, executeSETUDATAKS), "executeSETUDATAKS", false);
                break;

            case LOP_NAMECALLUDATA:
                lowerUserdataOpcode(i, offsetof(NativeContext, executeNAMECALLUDATA), "executeNAMECALLUDATA", true);
                break;

            case LOP_CLOSEUPVALS:
                lowerCloseUpvals(i, next);
                break;

            case LOP_COVERAGE:
                lowerCoverage(i, next);
                break;

            case LOP_JUMPIF:
                lowerTruthJump(i, LUAU_INSN_A(insn), LUAU_INSN_D(insn), next, false);
                break;

            case LOP_JUMPIFNOT:
                lowerTruthJump(i, LUAU_INSN_A(insn), LUAU_INSN_D(insn), next, true);
                break;

            case LOP_ADD:
                lowerBinOp(i, LUAU_INSN_A(insn), LUAU_INSN_B(insn), LUAU_INSN_C(insn), BinOp::Add, next);
                break;

            case LOP_SUB:
                lowerBinOp(i, LUAU_INSN_A(insn), LUAU_INSN_B(insn), LUAU_INSN_C(insn), BinOp::Sub, next);
                break;

            case LOP_MUL:
                lowerBinOp(i, LUAU_INSN_A(insn), LUAU_INSN_B(insn), LUAU_INSN_C(insn), BinOp::Mul, next);
                break;

            case LOP_DIV:
                lowerBinOp(i, LUAU_INSN_A(insn), LUAU_INSN_B(insn), LUAU_INSN_C(insn), BinOp::Div, next);
                break;

            case LOP_IDIV:
                lowerBinOp(i, LUAU_INSN_A(insn), LUAU_INSN_B(insn), LUAU_INSN_C(insn), BinOp::IDiv, next);
                break;

            case LOP_POW:
                lowerBinOp(i, LUAU_INSN_A(insn), LUAU_INSN_B(insn), LUAU_INSN_C(insn), BinOp::Pow, next);
                break;

            case LOP_MOD:
                lowerBinOp(i, LUAU_INSN_A(insn), LUAU_INSN_B(insn), LUAU_INSN_C(insn), BinOp::Mod, next);
                break;

            case LOP_ADDK:
            case LOP_SUBK:
            case LOP_MULK:
            case LOP_DIVK:
            case LOP_IDIVK:
            case LOP_MODK:
            case LOP_POWK:
            case LOP_SUBRK:
            case LOP_DIVRK:
                lowerBinOpK(i, next);
                break;

            case LOP_MINUS:
            {
                Value* rb = regPtrOf(base, int(LUAU_INSN_B(insn)));
                Value* tt = loadTT(rb);
                Value* fast = B.CreateICmpEQ(tt, B.getInt32(LUA_TNUMBER), "fast");

                BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
                B.CreateCondBr(fast, fastBlock, fallback);

                B.SetInsertPoint(fastBlock);
                storeNum(regPtrOf(base, int(LUAU_INSN_A(insn))), B.CreateFNeg(loadNum(rb)));
                B.CreateBr(blocks[next]);
                break;
            }

            case LOP_JUMPIFEQ:
                lowerCmpJump(i, LUAU_INSN_A(insn), auxOf(proto, i), LUAU_INSN_D(insn), next, false, true, false);
                break;

            case LOP_JUMPIFNOTEQ:
                lowerCmpJump(i, LUAU_INSN_A(insn), auxOf(proto, i), LUAU_INSN_D(insn), next, true, true, false);
                break;

            case LOP_JUMPIFLT:
                lowerCmpJump(i, LUAU_INSN_A(insn), auxOf(proto, i), LUAU_INSN_D(insn), next, false, false, true);
                break;

            case LOP_JUMPIFNOTLT:
                lowerCmpJump(i, LUAU_INSN_A(insn), auxOf(proto, i), LUAU_INSN_D(insn), next, true, false, true);
                break;

            case LOP_JUMPIFLE:
                lowerCmpJump(i, LUAU_INSN_A(insn), auxOf(proto, i), LUAU_INSN_D(insn), next, false, false, false);
                break;

            case LOP_JUMPIFNOTLE:
                lowerCmpJump(i, LUAU_INSN_A(insn), auxOf(proto, i), LUAU_INSN_D(insn), next, true, false, false);
                break;

            case LOP_JUMPXEQKNIL:
            case LOP_JUMPXEQKB:
            case LOP_JUMPXEQKN:
            case LOP_JUMPXEQKS:
                lowerJumpXEqK(i, next);
                break;

            case LOP_CMPPROTO:
            {
                Value* candidate = regPtrOf(base, int(LUAU_INSN_A(insn)));
                Value* isFunction = B.CreateICmpEQ(loadTT(candidate), B.getInt32(LUA_TFUNCTION), "isfunction");
                BasicBlock* inspect = BasicBlock::Create(B.getContext(), "cmpproto", fn);
                BasicBlock* mismatch = BasicBlock::Create(B.getContext(), "cmpproto_mismatch", fn);
                B.CreateCondBr(isFunction, inspect, mismatch);

                B.SetInsertPoint(inspect);
                Value* closure = toPtr(loadVal(candidate));
                Value* isLua = B.CreateICmpEQ(B.CreateLoad(B.getInt8Ty(), toPtr(addrOf(closure, offClosureIsC))), B.getInt8(0), "islua");
                BasicBlock* compare = BasicBlock::Create(B.getContext(), "cmpproto_lua", fn);
                B.CreateCondBr(isLua, compare, mismatch);

                B.SetInsertPoint(compare);
                Value* child = B.CreateLoad(ptrTy, toPtr(addrOf(closure, offClosureProto)), "prototype");
                Value* matches = B.CreateICmpEQ(B.CreateLoad(i32Ty, toPtr(addrOf(child, offProtoFunId))), B.getInt32(auxOf(proto, i)), "matches");
                B.CreateCondBr(matches, blocks[next], mismatch);

                B.SetInsertPoint(mismatch);
                BasicBlock* target = jumpTo(i, LUAU_INSN_D(insn), next);
                if (target)
                    B.CreateBr(target);
                break;
            }

            case LOP_FORNPREP:
            {
                // register layout: [limit, step, index]. The all-numeric fast
                // path performs no stores, only the conditional jump.
                const uint32_t a = LUAU_INSN_A(insn);

                Value* r0 = regPtrOf(base, int(a));
                Value* r1 = toPtr(addrOf(r0, sizeof(TValue)));
                Value* r2 = toPtr(addrOf(r0, 2 * sizeof(TValue)));

                Value* allNum = B.CreateAnd(
                    B.CreateICmpEQ(loadTT(r0), B.getInt32(LUA_TNUMBER)),
                    B.CreateAnd(B.CreateICmpEQ(loadTT(r1), B.getInt32(LUA_TNUMBER)), B.CreateICmpEQ(loadTT(r2), B.getInt32(LUA_TNUMBER))),
                    "allnum"
                );

                BasicBlock* fastBlock = BasicBlock::Create(B.getContext(), "fast", fn);
                B.CreateCondBr(allNum, fastBlock, fallback);

                B.SetInsertPoint(fastBlock);
                Value* limit = loadNum(r0);
                Value* step = loadNum(r1);
                Value* idxv = loadNum(r2);
                Value* stepPos = B.CreateFCmpOGT(step, ConstantFP::get(dblTy, 0.0), "steppos");
                Value* cont = B.CreateSelect(stepPos, B.CreateFCmpOLE(idxv, limit, "le"), B.CreateFCmpOLE(limit, idxv, "ge"), "cont");
                Value* jump = B.CreateNot(cont);

                const int32_t delta = LUAU_INSN_D(insn);
                if (delta == 0)
                    B.CreateBr(blocks[next]);
                else
                {
                    BasicBlock* target = jumpTo(i, delta, next);
                    if (target)
                        B.CreateCondBr(jump, target, blocks[next]);
                }
                break;
            }

            case LOP_FORNLOOP:
            {
                // register layout: [limit, step, index]. The VM checks the
                // interrupt callback on this backedge; native code defers to
                // the VM whenever an interrupt or GC safepoint is pending.
                const uint32_t a = LUAU_INSN_A(insn);

                Value* global = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateGlobal)), "global");
                Value* interrupt = B.CreateLoad(ptrTy, toPtr(addrOf(global, offGlobalCb + offCbInterrupt)), "interrupt");
                Value* noInterrupt = B.CreateIsNull(interrupt);

                BasicBlock* afterGc = BasicBlock::Create(B.getContext(), "aftergc", fn);
                if (FFlag::LuauBackedgeHeapCheck)
                {
                    BasicBlock* gcCheck = BasicBlock::Create(B.getContext(), "gck", fn);
                    B.CreateCondBr(noInterrupt, gcCheck, fallback);

                    B.SetInsertPoint(gcCheck);
                    Value* total = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalTotalBytes)), "total");
                    Value* threshold = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalGcThreshold)), "threshold");
                    B.CreateCondBr(B.CreateICmpSGE(total, threshold, "needgc"), fallback, afterGc);
                }
                else
                    B.CreateCondBr(noInterrupt, afterGc, fallback);

                B.SetInsertPoint(afterGc);
                Value* r0 = regPtrOf(base, int(a));
                Value* r1 = toPtr(addrOf(r0, sizeof(TValue)));
                Value* r2 = toPtr(addrOf(r0, 2 * sizeof(TValue)));

                Value* limit = loadNum(r0);
                Value* step = loadNum(r1);
                Value* idxv = B.CreateFAdd(loadNum(r2), step, "idx2");
                storeNum(r2, idxv);

                Value* stepPos = B.CreateFCmpOGT(step, ConstantFP::get(dblTy, 0.0), "steppos");
                Value* cont = B.CreateSelect(stepPos, B.CreateFCmpOLE(idxv, limit, "le"), B.CreateFCmpOLE(limit, idxv, "ge"), "cont");

                const int32_t delta = LUAU_INSN_D(insn);
                if (delta == 0)
                    B.CreateBr(blocks[next]);
                else
                {
                    BasicBlock* target = jumpTo(i, delta, next);
                    if (target)
                        B.CreateCondBr(cont, target, blocks[next]);
                }
                break;
            }

            case LOP_FORGPREP_INEXT:
                lowerForGPrepInext(i, next);
                break;

            case LOP_FORGPREP_NEXT:
                lowerForGPrepNext(i, next);
                break;

            case LOP_FORGPREP:
                lowerForGPrepTable(i, next);
                break;

            case LOP_FORGLOOP:
                if (int32_t(auxOf(proto, i)) < 0)
                    lowerForGLoopIpairs(i, next);
                else
                    lowerForGLoopPairs(i, next);
                break;

            case LOP_FASTCALL:
            case LOP_FASTCALL1:
            case LOP_FASTCALL2:
            case LOP_FASTCALL2K:
            case LOP_FASTCALL3:
                // The following MOVE/GETIMPORT + CALL sequence is the
                // specified slow path. Continue natively through it when no
                // builtin-specific LLVM expansion is selected.
                B.CreateBr(blocks[next]);
                break;

            default:
                // unhandled instruction: resume in the VM
                fb(i);
                break;
            }

            i = next;
        }

        // Multi-word bytecode instructions consume their AUX words in the
        // instruction-start block above.  The dispatcher still has one
        // resume block for every word because savedpc can be observed after a
        // yield or error path.  Leave no such block unterminated: route an
        // AUX-word resume (and any defensive uncovered word) through the VM.
        for (uint32_t blockIndex = 0; blockIndex < sizecode; ++blockIndex)
        {
            if (blocks[blockIndex]->getTerminator() == nullptr)
            {
                B.SetInsertPoint(blocks[blockIndex]);
                fb(blockIndex);
            }
        }

        std::string verifyMsg;
        raw_string_ostream os(verifyMsg);
        if (verifyFunction(*fn, &os))
        {
            error = name + " verification failed: " + verifyMsg;
            return false;
        }

        return true;
    }
};

} // namespace

bool lowerJitModule(void* moduleHandle, const std::vector<Proto*>& protos, std::string& error)
{
    Module* module = static_cast<Module*>(moduleHandle);
    if (!module)
    {
        error = "invalid module handle";
        return false;
    }

    std::vector<const Proto*> constProtos;
    constProtos.reserve(protos.size());
    for (Proto* proto : protos)
        constProtos.push_back(proto);

    Lowerer lowerer(module->getContext());
    if (!lowerer.lowerModule(*module, constProtos, error))
        return false;

    std::string verifyMsg;
    raw_string_ostream os(verifyMsg);
    if (verifyModule(*module, &os))
    {
        error = "module verification failed: " + verifyMsg;
        return false;
    }

    return true;
}

} // namespace CodeGen
} // namespace Luau

#else // !LUAU_USE_LLVM

namespace Luau
{
namespace CodeGen
{

bool lowerJitModule(void*, const std::vector<Proto*>&, std::string& error)
{
    error = "LLVM backend is not available in this build";
    return false;
}

} // namespace CodeGen
} // namespace Luau

#endif // LUAU_USE_LLVM
