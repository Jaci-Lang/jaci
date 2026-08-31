// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "CodeGenA64.h"

#include "Luau/AssemblyBuilderA64.h"
#include "Luau/UnwindBuilder.h"

#include "BitUtils.h"
#include "CodeGenContext.h"
#include "CodeGenUtils.h"
#include "NativeState.h"
#include "EmitCommonA64.h"

#include "lstate.h"

#ifdef CODEGEN_TARGET_A64_PTRAUTH_CALLS
#include <ptrauth.h>
#endif

LUAU_DYNAMIC_FASTFLAG(AddReturnExectargetCheck)
LUAU_FASTFLAG(LuauCIProto)

namespace Luau
{
namespace CodeGen
{
<<<<<<< HEAD
=======

>>>>>>> upstream/master
unsigned int getCpuFeaturesA64();

namespace A64
{

struct EntryLocations
{
    Label start;
    Label prologueEnd;
    Label epilogueStart;
};

static void emitExit(AssemblyBuilderA64& build, bool continueInVm)
{
    build.mov(x0, continueInVm);
    build.ldr(x1, mem(rNativeContext, offsetof(NativeContext, gateExit)));
    build.br(x1);
}

static void emitUpdatePcForExit(AssemblyBuilderA64& build)
{
    // x0 = pcpos * sizeof(Instruction)
    build.add(x0, rCode, x0);
    build.ldr(x1, mem(rState, offsetof(lua_State, ci)));
    build.str(x0, mem(x1, offsetof(CallInfo, savedpc)));
}

static void emitClearNativeFlag(AssemblyBuilderA64& build)
{
    build.ldr(x0, mem(rState, offsetof(lua_State, ci)));
    build.ldr(w1, mem(x0, offsetof(CallInfo, flags)));
    build.mov(w2, ~LUA_CALLINFO_NATIVE);
    build.and_(w1, w1, w2);
    build.str(w1, mem(x0, offsetof(CallInfo, flags)));
}

static void emitInterrupt(AssemblyBuilderA64& build)
{
    // x0 = pc offset (scaled by sizeof(Instruction))
    // x1 = return address in native code
    //
    // The inline fast-path (IrLoweringA64) no longer spills SSA values to the
    // stack before checking cb.interrupt, so this out-of-line helper is
    // responsible for preserving every AAPCS64 call-clobbered register around
    // the C call to `interrupt(L, -1)`. Luau-pinned registers (rState=x19,
    // rNativeContext=x20, rGlobalState=x21, rConstants=x22, rClosure=x23,
    // rCode=x24, rBase=x25) live in the x19..x28 callee-save range and are
    // therefore preserved by the C function itself.

    Label skip;
    Label resumeAndRestore;

    // Stash return address in rBase (callee-save, survives blr) so we can
    // return to it later; emitUpdateBase will refresh rBase from L->base.
    build.mov(rBase, x1);

    // Load interrupt handler; may be nullptr due to a race with the inline
    // check that brought us here; keep this test before the frame setup so
    // the race recovery path touches no extra stack.
    build.ldr(x2, mem(rState, offsetof(lua_State, global)));
    build.ldr(x2, mem(x2, offsetof(global_State, cb.interrupt)));
    build.cbz(x2, skip);

    // Frame layout (16-byte aligned, grows downward):
    //   sp + 496 .. sp + 511 : q31        (16 bytes, fully clobbered)
    //   ...
    //   sp + 240 .. sp + 255 : q16
    //   sp + 224 .. sp + 239 : q7
    //   ...
    //   sp + 112 .. sp + 127 : q0
    //   sp + 104 .. sp + 111 : x17
    //   ...
    //   sp +   0 .. sp +   7 : x0
    // Total = 144 (GPR x0..x17, 18 * 8) + 384 (Q 0..7 + 16..31, 24 * 16)
    //       = 528, which is 16-byte aligned.
    constexpr unsigned kFrameSize = 18 * 8 + 24 * 16;
    static_assert((kFrameSize & 0xfu) == 0, "emitInterrupt stack frame must be 16-byte aligned");

    constexpr unsigned kGprBase = 0;
    constexpr unsigned kQ0_Q7Base = 18 * 8;
    constexpr unsigned kQ16_Q31Base = 18 * 8 + 8 * 16;

    build.sub(sp, sp, uint16_t(kFrameSize));

    // Save x0..x17 in pairs
    build.stp(x0,  x1,  mem(sp, kGprBase + 0 * 8));
    build.stp(x2,  x3,  mem(sp, kGprBase + 2 * 8));
    build.stp(x4,  x5,  mem(sp, kGprBase + 4 * 8));
    build.stp(x6,  x7,  mem(sp, kGprBase + 6 * 8));
    build.stp(x8,  x9,  mem(sp, kGprBase + 8 * 8));
    build.stp(x10, x11, mem(sp, kGprBase + 10 * 8));
    build.stp(x12, x13, mem(sp, kGprBase + 12 * 8));
    build.stp(x14, x15, mem(sp, kGprBase + 14 * 8));
    build.stp(x16, x17, mem(sp, kGprBase + 16 * 8));

    // Save q0..q7 (fully, upper halves are call-clobbered)
    build.str(q0,  mem(sp, kQ0_Q7Base + 0 * 16));
    build.str(q1,  mem(sp, kQ0_Q7Base + 1 * 16));
    build.str(q2,  mem(sp, kQ0_Q7Base + 2 * 16));
    build.str(q3,  mem(sp, kQ0_Q7Base + 3 * 16));
    build.str(q4,  mem(sp, kQ0_Q7Base + 4 * 16));
    build.str(q5,  mem(sp, kQ0_Q7Base + 5 * 16));
    build.str(q6,  mem(sp, kQ0_Q7Base + 6 * 16));
    build.str(q7,  mem(sp, kQ0_Q7Base + 7 * 16));

    // Save q16..q31 (fully call-clobbered on AAPCS64)
    build.str(q16, mem(sp, kQ16_Q31Base + 0 * 16));
    build.str(q17, mem(sp, kQ16_Q31Base + 1 * 16));
    build.str(q18, mem(sp, kQ16_Q31Base + 2 * 16));
    build.str(q19, mem(sp, kQ16_Q31Base + 3 * 16));
    build.str(q20, mem(sp, kQ16_Q31Base + 4 * 16));
    build.str(q21, mem(sp, kQ16_Q31Base + 5 * 16));
    build.str(q22, mem(sp, kQ16_Q31Base + 6 * 16));
    build.str(q23, mem(sp, kQ16_Q31Base + 7 * 16));
    build.str(q24, mem(sp, kQ16_Q31Base + 8 * 16));
    build.str(q25, mem(sp, kQ16_Q31Base + 9 * 16));
    build.str(q26, mem(sp, kQ16_Q31Base + 10 * 16));
    build.str(q27, mem(sp, kQ16_Q31Base + 11 * 16));
    build.str(q28, mem(sp, kQ16_Q31Base + 12 * 16));
    build.str(q29, mem(sp, kQ16_Q31Base + 13 * 16));
    build.str(q30, mem(sp, kQ16_Q31Base + 14 * 16));
    build.str(q31, mem(sp, kQ16_Q31Base + 15 * 16));

    // Recompute stashed x0 (pc offset from entry) via rBase: we can't get the
    // original x0 back without adding extra save/restore slots before the
    // frame, so recompute it from L->ci->savedpc. Instead, recompute
    // savedpc directly using rCode (which is preserved by C) + the value
    // stashed at entry. rCode is callee-saved via x24 pin. Use a temp reg
    // (any call-clobbered one works, e.g. x0 itself before we overwrite it).
    //
    // We can still recover the original pc offset from the stack slot we
    // wrote x0 to; read it back now that we've saved everything.
    build.ldr(x0, mem(sp, kGprBase + 0 * 8));

    // Update L->ci->savedpc; required in case interrupt callback errors
    build.add(x0, rCode, x0);
    build.ldr(x1, mem(rState, offsetof(lua_State, ci)));
    build.str(x0, mem(x1, offsetof(CallInfo, savedpc)));

    // Call interrupt(L, -1); x2 already holds the handler pointer
    build.mov(x0, rState);
    build.mov(w1, -1);
    build.blr(x2);

    // Check if we need to exit (callback may have triggered an error).
    // Load status before restoring regs; exit path never resumes native.
    build.ldrb(w0, mem(rState, offsetof(lua_State, status)));
    build.cbz(w0, resumeAndRestore);

    // Error path: L->ci->savedpc-- then exit to VM.
    // Note: this path intentionally skips register restore; emitExit unwinds
    // to the interpreter cleanly, so no frame leak is observable.
    build.ldr(x1, mem(rState, offsetof(lua_State, ci)));
    build.ldr(x0, mem(x1, offsetof(CallInfo, savedpc)));
    build.sub(x0, x0, uint16_t(sizeof(Instruction)));
    build.str(x0, mem(x1, offsetof(CallInfo, savedpc)));

    emitExit(build, /* continueInVm */ false);

    build.setLabel(resumeAndRestore);

    // Restore q16..q31
    build.ldr(q16, mem(sp, kQ16_Q31Base + 0 * 16));
    build.ldr(q17, mem(sp, kQ16_Q31Base + 1 * 16));
    build.ldr(q18, mem(sp, kQ16_Q31Base + 2 * 16));
    build.ldr(q19, mem(sp, kQ16_Q31Base + 3 * 16));
    build.ldr(q20, mem(sp, kQ16_Q31Base + 4 * 16));
    build.ldr(q21, mem(sp, kQ16_Q31Base + 5 * 16));
    build.ldr(q22, mem(sp, kQ16_Q31Base + 6 * 16));
    build.ldr(q23, mem(sp, kQ16_Q31Base + 7 * 16));
    build.ldr(q24, mem(sp, kQ16_Q31Base + 8 * 16));
    build.ldr(q25, mem(sp, kQ16_Q31Base + 9 * 16));
    build.ldr(q26, mem(sp, kQ16_Q31Base + 10 * 16));
    build.ldr(q27, mem(sp, kQ16_Q31Base + 11 * 16));
    build.ldr(q28, mem(sp, kQ16_Q31Base + 12 * 16));
    build.ldr(q29, mem(sp, kQ16_Q31Base + 13 * 16));
    build.ldr(q30, mem(sp, kQ16_Q31Base + 14 * 16));
    build.ldr(q31, mem(sp, kQ16_Q31Base + 15 * 16));

    // Restore q0..q7
    build.ldr(q0,  mem(sp, kQ0_Q7Base + 0 * 16));
    build.ldr(q1,  mem(sp, kQ0_Q7Base + 1 * 16));
    build.ldr(q2,  mem(sp, kQ0_Q7Base + 2 * 16));
    build.ldr(q3,  mem(sp, kQ0_Q7Base + 3 * 16));
    build.ldr(q4,  mem(sp, kQ0_Q7Base + 4 * 16));
    build.ldr(q5,  mem(sp, kQ0_Q7Base + 5 * 16));
    build.ldr(q6,  mem(sp, kQ0_Q7Base + 6 * 16));
    build.ldr(q7,  mem(sp, kQ0_Q7Base + 7 * 16));

    // Restore x0..x17 in pairs
    build.ldp(x0,  x1,  mem(sp, kGprBase + 0 * 8));
    build.ldp(x2,  x3,  mem(sp, kGprBase + 2 * 8));
    build.ldp(x4,  x5,  mem(sp, kGprBase + 4 * 8));
    build.ldp(x6,  x7,  mem(sp, kGprBase + 6 * 8));
    build.ldp(x8,  x9,  mem(sp, kGprBase + 8 * 8));
    build.ldp(x10, x11, mem(sp, kGprBase + 10 * 8));
    build.ldp(x12, x13, mem(sp, kGprBase + 12 * 8));
    build.ldp(x14, x15, mem(sp, kGprBase + 14 * 8));
    build.ldp(x16, x17, mem(sp, kGprBase + 16 * 8));

    build.add(sp, sp, uint16_t(kFrameSize));

    build.setLabel(skip);

    // rBase stashed the return address at entry; interrupt callbacks may
    // reallocate the Lua stack so refresh rBase from L->base before jumping.
    // Return address goes to a scratch first; we use x0 which is dead after.
    build.mov(x0, rBase);
    emitUpdateBase(build);
    build.br(x0);
}

static void emitContinueCall(AssemblyBuilderA64& build, ModuleHelpers& helpers)
{
    // x0 = closure object to reentry (equal to clvalue(L->ci->func))

    // If the fallback yielded, we need to do this right away
    // note: it's slightly cheaper to check x0 LSB; a valid Closure pointer must be aligned to 8 bytes
    CODEGEN_ASSERT(CALL_FALLBACK_YIELD == 1);
    build.tbnz(x0, 0, helpers.exitNoContinueVm);

    // Need to update state of the current function before we jump away
    if (FFlag::LuauCIProto)
    {
        build.ldr(x1, mem(rState, offsetof(lua_State, ci)));
        build.ldr(x1, mem(x1, offsetof(CallInfo, p))); // L->ci->p aka proto
    }
    else
    {
        build.ldr(x1, mem(x0, offsetof(Closure, l.p))); // cl->l.p aka proto
    }

    build.ldr(x2, mem(x1, offsetof(Proto, exectarget)));
    build.cbz(x2, helpers.exitContinueVm);

    build.mov(rClosure, x0);

    static_assert(offsetof(Proto, code) == offsetof(Proto, k) + sizeof(Proto::k));
    build.ldp(rConstants, rCode, mem(x1, offsetof(Proto, k))); // proto->k, proto->code

    build.br(x2);
}

void emitReturn(AssemblyBuilderA64& build, ModuleHelpers& helpers)
{
    // x1 = res
    // w2 = number of written values

    // x0 = ci
    build.ldr(x0, mem(rState, offsetof(lua_State, ci)));
    // w3 = ci->nresults
    build.ldr(w3, mem(x0, offsetof(CallInfo, nresults)));

    Label skipResultCopy;

    // Fill the rest of the expected results (nresults - written) with 'nil'
    build.cmp(w2, w3);
    build.b(ConditionA64::GreaterEqual, skipResultCopy);

    // TODO: cmp above could compute this and flags using subs
    build.sub(w2, w3, w2); // counter = nresults - written
    build.mov(w4, LUA_TNIL);

    Label repeatNilLoop = build.setLabel();
    build.str(w4, mem(x1, offsetof(TValue, tt)));
    build.add(x1, x1, uint16_t(sizeof(TValue)));
    build.sub(w2, w2, uint16_t(1));
    build.cbnz(w2, repeatNilLoop);

    build.setLabel(skipResultCopy);

    // x2 = cip = ci - 1
    build.sub(x2, x0, uint16_t(sizeof(CallInfo)));

    // res = cip->top when nresults >= 0
    Label skipFixedRetTop;
    build.tbnz(w3, 31, skipFixedRetTop);
    build.ldr(x1, mem(x2, offsetof(CallInfo, top))); // res = cip->top
    build.setLabel(skipFixedRetTop);

    // Update VM state (ci, base, top)
    build.str(x2, mem(rState, offsetof(lua_State, ci)));      // L->ci = cip
    build.ldr(rBase, mem(x2, offsetof(CallInfo, base)));      // sync base = L->base while we have a chance
    build.str(rBase, mem(rState, offsetof(lua_State, base))); // L->base = cip->base

    build.str(x1, mem(rState, offsetof(lua_State, top))); // L->top = res

    // Unlikely, but this might be the last return from VM
    build.ldr(w4, mem(x0, offsetof(CallInfo, flags)));
    build.tbnz(w4, countrz(uint32_t(LUA_CALLINFO_RETURN)), helpers.exitNoContinueVm);

    // Continue in interpreter if function has no native data
    build.ldr(w4, mem(x2, offsetof(CallInfo, flags)));
    build.tbz(w4, countrz(uint32_t(LUA_CALLINFO_NATIVE)), helpers.exitContinueVm);

    // Need to update state of the current function before we jump away
    build.ldr(rClosure, mem(x2, offsetof(CallInfo, func)));
    build.ldr(rClosure, mem(rClosure, offsetof(TValue, value.gc)));

    if (FFlag::LuauCIProto)
        build.ldr(x1, mem(x2, offsetof(CallInfo, p))); // ci->p aka proto
    else
        build.ldr(x1, mem(rClosure, offsetof(Closure, l.p))); // cl->l.p aka proto

    if (DFFlag::AddReturnExectargetCheck)
    {
        // Get new instruction location
        static_assert(offsetof(Proto, exectarget) == offsetof(Proto, execdata) + sizeof(Proto::execdata));
        build.ldp(x3, x4, mem(x1, offsetof(Proto, execdata)));
        build.cbz(x4, helpers.exitContinueVmClearNativeFlag);
    }

    static_assert(offsetof(Proto, code) == offsetof(Proto, k) + sizeof(Proto::k));
    build.ldp(rConstants, rCode, mem(x1, offsetof(Proto, k))); // proto->k, proto->code

    // Get instruction index from instruction pointer
    // To get instruction index from instruction pointer, we need to divide byte offset by 4
    // But we will actually need to scale instruction index by 4 back to byte offset later so it cancels out
    build.ldr(x2, mem(x2, offsetof(CallInfo, savedpc))); // cip->savedpc
    build.sub(x2, x2, rCode);

    if (!DFFlag::AddReturnExectargetCheck)
    {
        // Get new instruction location and jump to it
        static_assert(offsetof(Proto, exectarget) == offsetof(Proto, execdata) + sizeof(Proto::execdata));
        build.ldp(x3, x4, mem(x1, offsetof(Proto, execdata)));
    }
    build.ldr(w2, mem(x3, x2));
    build.add(x4, x4, x2);
    build.br(x4);
}

static EntryLocations buildEntryFunction(AssemblyBuilderA64& build, UnwindBuilder& unwind)
{
    EntryLocations locations;

    // Arguments: x0 = lua_State*, x1 = Proto*, x2 = native code pointer to jump to, x3 = NativeContext*

    locations.start = build.setLabel();

    // prologue
    if (build.features & Feature_PtrAuthRet)
        build.pacibsp();

    build.sub(sp, sp, uint16_t(kStackSize));
    build.stp(x29, x30, mem(sp)); // fp, lr

    // stash non-volatile registers used for execution environment
    build.stp(x19, x20, mem(sp, 16));
    build.stp(x21, x22, mem(sp, 32));
    build.stp(x23, x24, mem(sp, 48));
    build.str(x25, mem(sp, 64));

    build.mov(x29, sp); // this is only necessary if we maintain frame pointers, which we do in the JIT for now

    locations.prologueEnd = build.setLabel();

    uint32_t prologueSize = build.getLabelOffset(locations.prologueEnd) - build.getLabelOffset(locations.start);

    // Setup native execution environment
    build.mov(rState, x0);
    build.mov(rNativeContext, x3);
    build.ldr(rGlobalState, mem(x0, offsetof(lua_State, global)));

    build.ldr(rBase, mem(x0, offsetof(lua_State, base))); // L->base

    static_assert(offsetof(Proto, code) == offsetof(Proto, k) + sizeof(Proto::k));
    build.ldp(rConstants, rCode, mem(x1, offsetof(Proto, k))); // proto->k, proto->code

    build.ldr(x9, mem(x0, offsetof(lua_State, ci)));          // L->ci
    build.ldr(x9, mem(x9, offsetof(CallInfo, func)));         // L->ci->func
    build.ldr(rClosure, mem(x9, offsetof(TValue, value.gc))); // L->ci->func->value.gc aka cl

    // Jump to the specified instruction; further control flow will be handled with custom ABI with register setup from EmitCommonA64.h
    build.br(x2);

    // Even though we jumped away, we will return here in the end
    locations.epilogueStart = build.setLabel();

    // Cleanup and exit
    build.ldr(x25, mem(sp, 64));
    build.ldp(x23, x24, mem(sp, 48));
    build.ldp(x21, x22, mem(sp, 32));
    build.ldp(x19, x20, mem(sp, 16));
    build.ldp(x29, x30, mem(sp)); // fp, lr
    build.add(sp, sp, uint16_t(kStackSize));

    if (build.features & Feature_PtrAuthRet)
<<<<<<< HEAD
        build.retab();
=======
        build.retab(); // Authenticate the LR signed by pacibsp in the prologue, then return
>>>>>>> upstream/master
    else
        build.ret();

    // Our entry function is special, it spans the whole remaining code area
    unwind.startFunction();
    unwind.prologueA64(prologueSize, kStackSize, {x29, x30, x19, x20, x21, x22, x23, x24, x25});
    unwind.finishFunction(build.getLabelOffset(locations.start), kFullBlockFunction);

    return locations;
}

bool initHeaderFunctions(BaseCodeGenContext& codeGenContext)
{
<<<<<<< HEAD
#if defined(CODEGEN_TARGET_A64)
    AssemblyBuilderA64 build(/* logger= */ nullptr, getCpuFeaturesA64());
=======
    // This file is built for every target, but CodeGen.cpp only defines
    // getCpuFeaturesA64() when the host is arm64. The gate is only executed on
    // an arm64 host, so the features are irrelevant elsewhere.
#if defined(CODEGEN_TARGET_A64)
    AssemblyBuilderA64 build(/* logger= */ nullptr, /* features= */ getCpuFeaturesA64());
>>>>>>> upstream/master
#else
    AssemblyBuilderA64 build(/* logger= */ nullptr, /* features= */ 0);
#endif
    UnwindBuilder& unwind = *codeGenContext.unwindBuilder.get();

    unwind.startInfo(UnwindBuilder::A64);

    EntryLocations entryLocations = buildEntryFunction(build, unwind);

    build.finalize();

    unwind.finishInfo();

    CODEGEN_ASSERT(build.data.empty());

    codeGenContext.gateAllocationData = codeGenContext.codeAllocator.allocate(
        build.data.data(), int(build.data.size()), reinterpret_cast<const uint8_t*>(build.code.data()), int(build.code.size() * sizeof(build.code[0]))
    );

    if (!codeGenContext.gateAllocationData.start)
        return false;

    uint8_t* codeStart = codeGenContext.gateAllocationData.codeStart;

    // Set the offset at the beginning so that functions in new blocks will not overlay the locations
    // specified by the unwind information of the entry function
    unwind.setBeginOffset(build.getLabelOffset(entryLocations.prologueEnd));

    uint8_t* gateEntry = codeStart + build.getLabelOffset(entryLocations.start);

#ifdef CODEGEN_TARGET_A64_PTRAUTH_CALLS
<<<<<<< HEAD
=======
    // onEnter() invokes gateEntry through a GateFn function pointer.  When PAC
    // function pointer signing is enabled, we need to sign the function pointer
    // so that authentication succeeds when onEnter() calls it.
>>>>>>> upstream/master
    gateEntry = (uint8_t*)ptrauth_sign_unauthenticated(gateEntry, ptrauth_key_function_pointer, 0);
#endif

    codeGenContext.context.gateEntry = gateEntry;
    codeGenContext.context.gateExit = codeStart + build.getLabelOffset(entryLocations.epilogueStart);

    return true;
}

void assembleHelpers(LogBuilder* logger, AssemblyBuilderA64& build, ModuleHelpers& helpers)
{
    if (logger)
        logger->append("; updatePcAndContinueInVm\n");
    build.setLabel(helpers.updatePcAndContinueInVm);
    emitUpdatePcForExit(build);

    if (logger)
        logger->append("; exitContinueVmClearNativeFlag\n");
    build.setLabel(helpers.exitContinueVmClearNativeFlag);
    emitClearNativeFlag(build);

    if (logger)
        logger->append("; exitContinueVm\n");
    build.setLabel(helpers.exitContinueVm);
    emitExit(build, /* continueInVm */ true);

    if (logger)
        logger->append("; exitNoContinueVm\n");
    build.setLabel(helpers.exitNoContinueVm);
    emitExit(build, /* continueInVm */ false);

    if (logger)
        logger->append("; interrupt\n");
    build.setLabel(helpers.interrupt);
    emitInterrupt(build);

    if (logger)
        logger->append("; return\n");
    build.setLabel(helpers.return_);
    emitReturn(build, helpers);

    if (logger)
        logger->append("; continueCall\n");
    build.setLabel(helpers.continueCall);
    emitContinueCall(build, helpers);
}

} // namespace A64
} // namespace CodeGen
} // namespace Luau
