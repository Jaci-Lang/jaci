// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee
#include "LlvmJit.h"

#include "Luau/Bytecode.h"
#include "Luau/CodeGenCommon.h"
#include "Luau/Common.h"
#include "Luau/LlvmEngine.h"

#include "lstate.h"
#include "lobject.h"
#include "lgc.h"

#include <cmath>

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#if LUAU_USE_LLVM

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
using llvm::GlobalVariable;
using llvm::GlobalValue;
using llvm::IRBuilder;
using llvm::LLVMContext;
using llvm::Module;
using llvm::SwitchInst;
using llvm::Type;
using llvm::Value;
using llvm::verifyFunction;
using llvm::verifyModule;
using llvm::raw_string_ostream;

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

    default:
        return 1;
    }
}

// Lowers one proto to a dso_local function with one resume block per bytecode
// word. Wave 1 fast paths are emitted inline; every other instruction falls
// back to the VM (savedpc is set to the instruction, the function returns 1).
//
// The register file is the VM's own stack (L->ci->base); the native code
// reads and writes TValues in place. The only C call emitted (luaC_step,
// guarded by a needsGC check at loop backedges) never reallocates the stack,
// so the base loaded at entry stays valid for the whole function.
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
        , offCiBase(offsetof(CallInfo, base))
        , offCiSavedPc(offsetof(CallInfo, savedpc))
        , offProtoCode(offsetof(Proto, code))
        , offProtoK(offsetof(Proto, k))
        , offGlobalCb(offsetof(global_State, cb))
        , offGlobalTotalBytes(offsetof(global_State, totalbytes))
        , offGlobalGcThreshold(offsetof(global_State, GCthreshold))
        , offCbInterrupt(offsetof(lua_Callbacks, interrupt))
    {}

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

    uint64_t offLStateGlobal, offLStateCi;
    uint64_t offCiBase, offCiSavedPc;
    uint64_t offProtoCode, offProtoK;
    uint64_t offGlobalCb, offGlobalTotalBytes, offGlobalGcThreshold;
    uint64_t offCbInterrupt;

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

        // External function-pointer entry for host calls (luaC_step): the JIT
        // object is not linked into the process, so host symbols are passed as
        // absolute address constants (static relocation model, single process).
        GlobalVariable* extLuaCStep = new GlobalVariable(
            module,
            ptrTy,
            false,
            GlobalValue::PrivateLinkage,
            ConstantExpr::getIntToPtr(B.getInt64(reinterpret_cast<uintptr_t>(luaC_step)), ptrTy),
            "luau_jit_ext_luaC_step"
        );
        GlobalVariable* extFloor = new GlobalVariable(
            module,
            ptrTy,
            false,
            GlobalValue::PrivateLinkage,
            ConstantExpr::getIntToPtr(B.getInt64(reinterpret_cast<uintptr_t>(::floor)), ptrTy),
            "luau_jit_ext_floor"
        );
        GlobalVariable* extPow = new GlobalVariable(
            module,
            ptrTy,
            false,
            GlobalValue::PrivateLinkage,
            ConstantExpr::getIntToPtr(B.getInt64(reinterpret_cast<uintptr_t>(::pow)), ptrTy),
            "luau_jit_ext_pow"
        );

        Value* L = fn->getArg(0);
        Value* p = fn->getArg(1);

        // entry: compute the resume index from L->ci->savedpc
        BasicBlock* entry = BasicBlock::Create(B.getContext(), "entry", fn);
        B.SetInsertPoint(entry);

        Value* fbSlot = B.CreateAlloca(i64Ty, nullptr, "fbidx");
        B.CreateStore(B.getInt64(0), fbSlot);

        Value* ci = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateCi)), "ci");
        Value* base = B.CreateLoad(ptrTy, toPtr(addrOf(ci, offCiBase)), "base");
        Value* code = B.CreateLoad(ptrTy, toPtr(addrOf(p, offProtoCode)), "code");
        Value* savedPc = B.CreateLoad(ptrTy, toPtr(addrOf(ci, offCiSavedPc)), "savedpc");

        Value* idx = B.CreateAShr(
            B.CreateSub(B.CreatePtrToInt(savedPc, i64Ty), B.CreatePtrToInt(code, i64Ty)),
            B.getInt64(sizeof(Instruction)),
            "idx"
        );
        Value* inRange = B.CreateICmpSLT(idx, B.getInt64(sizecode), "inrange");

        BasicBlock* dispatch = BasicBlock::Create(B.getContext(), "dispatch", fn);
        BasicBlock* fallback = BasicBlock::Create(B.getContext(), "fallback", fn);
        BasicBlock* exitNoop = BasicBlock::Create(B.getContext(), "exit", fn);
        B.CreateCondBr(inRange, dispatch, exitNoop);

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
            case BinOp::Mod:
            {
                // luai_nummod: a - floor(a / b) * b
                Value* flFn = B.CreateLoad(ptrTy, extFloor);
                Value* fl = B.CreateCall(dblFn1Ty(), flFn, {B.CreateFDiv(nb, nc)});
                r = B.CreateFSub(nb, B.CreateFMul(fl, nc));
                break;
            }
            default:
            {
                // Pow: libm pow, exactly as the VM (luai_numpow)
                Value* pwFn = B.CreateLoad(ptrTy, extPow);
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
            Value* kv = toPtr(B.CreateMul(B.CreatePtrToInt(k, i64Ty), B.getInt64(sizeof(TValue) * cidx), "kvo"));

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
            case LOP_MODK:
            {
                Value* flFn = B.CreateLoad(ptrTy, extFloor);
                Value* fl = B.CreateCall(dblFn1Ty(), flFn, {B.CreateFDiv(nr, nk)});
                r = B.CreateFSub(nr, B.CreateFMul(fl, nk));
                break;
            }
            case LOP_POWK:
            {
                Value* pwFn = B.CreateLoad(ptrTy, extPow);
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
            const uint32_t aux = proto->code[i + 1];

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

        // operand decoding walk
        uint32_t i = 0;
        while (i < sizecode)
        {
            const uint32_t insn = proto->code[i];
            const uint8_t op = LUAU_INSN_OP(insn);
            const uint32_t words = instructionWords(proto, insn);
            const uint32_t next = i + words;

            B.SetInsertPoint(blocks[i]);

            switch (op)
            {
            case LOP_NOP:
            {
                B.CreateBr(blocks[next]);
                break;
            }

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
                    rc = toPtr(B.CreateMul(B.CreatePtrToInt(k, i64Ty), B.getInt64(sizeof(TValue) * LUAU_INSN_C(insn)), "kvo"));
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
                    rc = toPtr(B.CreateMul(B.CreatePtrToInt(k, i64Ty), B.getInt64(sizeof(TValue) * LUAU_INSN_C(insn)), "kvo"));
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
                // interrupt callback on this backedge; a native execution must
                // honor the same safepoint, so an installed interrupt defers to
                // the VM. The needsGC check mirrors VM_CHECK_GC so a pending
                // GC step still runs on the backedge (luaC_step never
                // reallocates the stack, so base stays valid).
                const uint32_t a = LUAU_INSN_A(insn);

                Value* global = B.CreateLoad(ptrTy, toPtr(addrOf(L, offLStateGlobal)), "global");
                Value* interrupt = B.CreateLoad(ptrTy, toPtr(addrOf(global, offGlobalCb + offCbInterrupt)), "interrupt");
                Value* noInterrupt = B.CreateIsNull(interrupt);

                BasicBlock* gcCheck = BasicBlock::Create(B.getContext(), "gck", fn);
                B.CreateCondBr(noInterrupt, gcCheck, fallback);

                B.SetInsertPoint(gcCheck);
                Value* total = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalTotalBytes)), "total");
                Value* threshold = B.CreateLoad(i64Ty, toPtr(addrOf(global, offGlobalGcThreshold)), "threshold");
                Value* needGc = B.CreateICmpSGE(total, threshold, "needgc");
                BasicBlock* doGc = BasicBlock::Create(B.getContext(), "dogc", fn);
                BasicBlock* afterGc = BasicBlock::Create(B.getContext(), "aftergc", fn);
                B.CreateCondBr(needGc, doGc, afterGc);

                B.SetInsertPoint(doGc);
                {
                    FunctionType* stepTy = FunctionType::get(B.getVoidTy(), {ptrTy, i1Ty}, false);
                    Value* stepFn = B.CreateLoad(ptrTy, extLuaCStep, "stepfn");
                    B.CreateCall(stepTy, stepFn, {L, B.getInt1(1)});
                }
                B.CreateBr(afterGc);

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

            default:
                // unhandled instruction: resume in the VM
                fb(i);
                break;
            }

            i = next;
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
