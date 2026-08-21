// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/CodeGen.h"
#include "Luau/IrAnalysis.h"
#include "Luau/IrBuilder.h"
#include "Luau/IrDump.h"

#include "doctest.h"
#include "ScopedFlags.h"

#include <regex>

LUAU_FASTFLAG(LuauCodegenDseRestoreHints)
LUAU_FASTFLAG(LuauCodegenDseRestoreHintUpdate)

using namespace Luau::CodeGen;

static void stripLinesContaining(std::string& text, const char* needle)
{
    size_t pos = 0;

    while ((pos = text.find(needle, pos)) != std::string::npos)
    {
        size_t lineStart = text.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;

        size_t lineEnd = text.find('\n', pos);

        if (lineEnd == std::string::npos)
            text.erase(lineStart);
        else
            text.erase(lineStart, lineEnd - lineStart + 1);

        pos = lineStart;
    }
}

// To not have to update results every time a new field is added to lua_State/global_State, we replace the offsets
static void normalizeStateOffsets(std::string& text)
{
    std::string result;
    result.reserve(text.size());

    std::string pendingReg;

    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();

        std::string line = text.substr(pos, eol - pos);

        std::smatch match;
        if (std::regex_search(line, match, std::regex(R"((\w+),.*\[r15\+[^\]]+\])")))
        {
            pendingReg = match[1].str();
            line = std::regex_replace(line, std::regex(R"(\[r15\+[^\]]+\])"), "[r15+<offset>]");
        }
        else if (!pendingReg.empty())
        {
            std::regex deref("\\[" + pendingReg + "\\+[^\\]]+\\]");
            line = std::regex_replace(line, deref, "[" + pendingReg + "+<offset>]");
        }

        result += line;
        if (eol < text.size())
            result += '\n';
        pos = eol + 1;
    }

    text = std::move(result);
}

class IrAssemblyFixture
{
public:
    IrAssemblyFixture()
        : build(hooks)
    {
        options.target = AssemblyOptions::X64_Windows;

        options.outputBinary = false;

        options.includeAssembly = true;
        options.includeIr = true;
        options.includeOutlinedCode = false;
        options.includeIrTypes = true;

        options.includeIrPrefix = IncludeIrPrefix::No;
        options.includeUseInfo = IncludeUseInfo::No;
        options.includeCfgInfo = IncludeCfgInfo::No;
        options.includeRegFlowInfo = IncludeRegFlowInfo::No;
    }

    std::string lower()
    {
        std::string text = getAssemblyFromIr(build, options);
        stripLinesContaining(text, "; skipping ");
        normalizeStateOffsets(text);

        return text;
    }

    HostIrHooks hooks;
    IrBuilder build;
    AssemblyOptions options;

    // Luau.VM headers are not accessible
    int tnil = parseTagName("tnil");
    int tboolean = parseTagName("tboolean");
    int tnumber = parseTagName("tnumber");
    int tinteger = parseTagName("tinteger");
    int tvector = parseTagName("tvector");
    int tstring = parseTagName("tstring");
    int ttable = parseTagName("ttable");
    int tfunction = parseTagName("tfunction");
    int tuserdata = parseTagName("tuserdata");
    int tbuffer = parseTagName("tbuffer");
};

TEST_SUITE_BEGIN("IrAssembly");

TEST_CASE_FIXTURE(IrAssemblyFixture, "PreserveIntChainedFromDoubleVmReg")
{
    IrOp entry = build.block(IrBlockKind::Internal);

    build.beginBlock(entry);
    IrOp d = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(1));
    IrOp i = build.inst(IrCmd::NUM_TO_INT, d);
    build.inst(IrCmd::INTERRUPT, build.constUint(0));
    build.inst(IrCmd::STORE_INT, build.vmReg(0), i);
    build.inst(IrCmd::STORE_TAG, build.vmReg(0), build.constTag(tboolean));
    build.inst(IrCmd::RETURN, build.vmReg(0), build.constInt(1));
    updateUseCounts(build.function);

    // %1 is kept in eax across the inline interrupt check; out-of-line
    // emitInterrupt saves/restores every call-clobbered reg around the C
    // callback, so no recompute from R1 is required on the fast path.
    CHECK_EQ(
        "\n" + lower(),
        R"(
; align 32 using ud2
bb_0:
.L12:
  %0 = LOAD_DOUBLE R1
 vmovsd      xmm0,qword ptr [r14+010h]
  %1 = NUM_TO_INT %0
 vcvttsd2si  eax,xmm0
  INTERRUPT 0u
 mov         rdx,qword ptr [r15+<offset>]
 cmp         qword ptr [rdx+<offset>],0
 jne         .L13
.L14:
  STORE_INT R0, %1
 mov         dword ptr [r14],eax
  STORE_TAG R0, tboolean
 mov         dword ptr [r14+0Ch],1
  RETURN R0, 1i
 vmovups     xmm0,xmmword ptr [r14]
 vmovups     xmmword ptr [r14-010h],xmm0
 mov         rdi,r14
 mov         ecx,1
 jmp         .L8

)"
    );
}

TEST_CASE_FIXTURE(IrAssemblyFixture, "PreserveIntChainedFromDoubleVmRegBoth")
{
    IrOp entry = build.block(IrBlockKind::Internal);

    build.beginBlock(entry);
    IrOp d = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(2));
    IrOp i = build.inst(IrCmd::NUM_TO_INT, d);
    build.inst(IrCmd::INTERRUPT, build.constUint(0));
    build.inst(IrCmd::STORE_INT, build.vmReg(0), i);
    build.inst(IrCmd::STORE_TAG, build.vmReg(0), build.constTag(tboolean));
    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(1), d);
    build.inst(IrCmd::STORE_TAG, build.vmReg(1), build.constTag(tnumber));
    build.inst(IrCmd::RETURN, build.vmReg(0), build.constInt(2));
    updateUseCounts(build.function);

    // Both %0 (xmm0) and %1 (eax) are preserved across the inline interrupt
    // check; out-of-line emitInterrupt saves/restores clobbered registers.
    CHECK_EQ(
        "\n" + lower(),
        R"(
; align 32 using ud2
bb_0:
.L12:
  %0 = LOAD_DOUBLE R2
 vmovsd      xmm0,qword ptr [r14+020h]
  %1 = NUM_TO_INT %0
 vcvttsd2si  eax,xmm0
  INTERRUPT 0u
 mov         rdx,qword ptr [r15+<offset>]
 cmp         qword ptr [rdx+<offset>],0
 jne         .L13
.L14:
  STORE_INT R0, %1
 mov         dword ptr [r14],eax
  STORE_TAG R0, tboolean
 mov         dword ptr [r14+0Ch],1
  STORE_DOUBLE R1, %0
 vmovsd      qword ptr [r14+010h],xmm0
  STORE_TAG R1, tnumber
 mov         dword ptr [r14+01Ch],3
  RETURN R0, 2i
 lea         rdi,[r14-010h]
 vmovups     xmm0,xmmword ptr [r14]
 vmovups     xmmword ptr [rdi],xmm0
 vmovups     xmm0,xmmword ptr [r14+010h]
 vmovups     xmmword ptr [rdi+010h],xmm0
 add         rdi,20h
 mov         ecx,2
 jmp         .L8

)"
    );
}

TEST_CASE_FIXTURE(IrAssemblyFixture, "PreserveIntWithoutChainSpillsToStack")
{
    IrOp entry = build.block(IrBlockKind::Internal);

    build.beginBlock(entry);
    IrOp a = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(1));
    IrOp b = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(2));
    IrOp sum = build.inst(IrCmd::ADD_NUM, a, b);
    IrOp i = build.inst(IrCmd::NUM_TO_INT, sum);
    build.inst(IrCmd::INTERRUPT, build.constUint(0));
    build.inst(IrCmd::STORE_INT, build.vmReg(3), i);
    build.inst(IrCmd::RETURN, build.vmReg(0), build.constInt(0));
    updateUseCounts(build.function);

    // %3 is kept in eax across the inline interrupt check; no stack spill
    // needed because the out-of-line emitInterrupt handler saves/restores
    // all call-clobbered registers around the C callback.
    CHECK_EQ(
        "\n" + lower(),
        R"(
; align 32 using ud2
bb_0:
.L12:
  %0 = LOAD_DOUBLE R1
 vmovsd      xmm0,qword ptr [r14+010h]
  %2 = ADD_NUM %0, R2
 vaddsd      xmm0,xmm0,qword ptr [r14+020h]
  %3 = NUM_TO_INT %2
 vcvttsd2si  eax,xmm0
  INTERRUPT 0u
 mov         rdx,qword ptr [r15+<offset>]
 cmp         qword ptr [rdx+<offset>],0
 jne         .L13
.L14:
  STORE_INT R3, %3
 mov         dword ptr [r14+030h],eax
  RETURN R0, 0i
 lea         rdi,[r14-010h]
 xor         ecx,ecx
 jmp         .L8

)"
    );
}

TEST_CASE_FIXTURE(IrAssemblyFixture, "DseHintMaterializesIntIntoDeadVmReg")
{
    ScopedFastFlag luauCodegenDseRestoreHints{FFlag::LuauCodegenDseRestoreHints, true};

    IrOp entry = build.block(IrBlockKind::Internal);
    build.beginBlock(entry);

    IrOp d = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(1));
    IrOp i = build.inst(IrCmd::NUM_TO_INT, d);

    // Kill R1 as a potential restore location
    IrOp doubled = build.inst(IrCmd::ADD_NUM, d, d);
    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(1), doubled);
    build.inst(IrCmd::STORE_TAG, build.vmReg(1), build.constTag(tnumber));

    // Prepare R4 store what will be removed by DSE, but preserved as a lazy restore location
    IrOp roundtrip = build.inst(IrCmd::INT_TO_NUM, i);
    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(4), roundtrip);
    build.inst(IrCmd::STORE_TAG, build.vmReg(4), build.constTag(tnumber));

    build.inst(IrCmd::INTERRUPT, build.constUint(0));

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(2), roundtrip);
    build.inst(IrCmd::STORE_TAG, build.vmReg(2), build.constTag(tnumber));

    build.inst(IrCmd::RETURN, build.vmReg(1), build.constInt(2));
    updateUseCounts(build.function);

    // %5 (xmm0) is preserved across the inline interrupt check; out-of-line
    // emitInterrupt saves/restores clobbered regs, so no DSE-hint spill to
    // R4 is needed; xmm0 is kept alive directly.
    CHECK_EQ(
        "\n" + lower(),
        R"(
; align 32 using ud2
bb_0:
.L12:
  %0 = LOAD_DOUBLE R1
 vmovsd      xmm0,qword ptr [r14+010h]
  %1 = NUM_TO_INT %0
 vcvttsd2si  eax,xmm0
  %2 = ADD_NUM %0, %0
 vaddsd      xmm0,xmm0,xmm0
  STORE_DOUBLE R1, %2
 vmovsd      qword ptr [r14+010h],xmm0
  STORE_TAG R1, tnumber
 mov         dword ptr [r14+01Ch],3
  %5 = INT_TO_NUM %1
 vcvtsi2sd   xmm0,xmm0,eax
  INTERRUPT 0u
 mov         rax,qword ptr [r15+<offset>]
 cmp         qword ptr [rax+<offset>],0
 jne         .L13
.L14:
  STORE_DOUBLE R2, %5
 vmovsd      qword ptr [r14+020h],xmm0
  STORE_TAG R2, tnumber
 mov         dword ptr [r14+02Ch],3
  RETURN R1, 2i
 lea         rdi,[r14-010h]
 vmovups     xmm0,xmmword ptr [r14+010h]
 vmovups     xmmword ptr [rdi],xmm0
 vmovups     xmm0,xmmword ptr [r14+020h]
 vmovups     xmmword ptr [rdi+010h],xmm0
 add         rdi,20h
 mov         ecx,2
 jmp         .L8

)"
    );
}

TEST_CASE_FIXTURE(IrAssemblyFixture, "DseHintCorruptsTagOnPartialValueKill")
{
    ScopedFastFlag luauCodegenDseRestoreHints{FFlag::LuauCodegenDseRestoreHints, true};

    IrOp entry = build.block(IrBlockKind::Internal);
    build.beginBlock(entry);

    build.inst(IrCmd::CHECK_TAG, build.inst(IrCmd::LOAD_TAG, build.vmReg(1)), build.constTag(tnumber), build.vmExit(0));
    build.inst(IrCmd::CHECK_TAG, build.inst(IrCmd::LOAD_TAG, build.vmReg(2)), build.constTag(tnumber), build.vmExit(0));

    IrOp r1Val = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(1));
    IrOp r2Val = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(2));
    IrOp computed = build.inst(IrCmd::ADD_NUM, r1Val, r2Val);

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(1), computed);
    build.inst(IrCmd::STORE_TAG, build.vmReg(1), build.constTag(tnumber)); // Will be removed as redundant

    build.inst(IrCmd::INTERRUPT, build.constUint(0)); // Trigger a spill

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(3), computed);
    build.inst(IrCmd::STORE_TAG, build.vmReg(3), build.constTag(tnumber));

    build.inst(IrCmd::CHECK_TAG, build.inst(IrCmd::LOAD_TAG, build.vmReg(4)), build.constTag(tnumber), build.vmExit(0));
    IrOp newVal = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(4));

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(1), newVal);
    build.inst(IrCmd::STORE_TAG, build.vmReg(1), build.constTag(tnumber)); // Will be removed as redundant

    build.inst(IrCmd::RETURN, build.vmReg(1), build.constInt(3));
    updateUseCounts(build.function);

    // No redundant tag store + partial kill means DSE hint for R1 is not
    // usable; %6 is kept in xmm0 across the inline interrupt fast-path.
    // Out-of-line emitInterrupt saves/restores xmm0 around the C callback.
    CHECK_EQ(
        "\n" + lower(),
        R"(
; align 32 using ud2
bb_0:
.L12:
  CHECK_TAG R1, tnumber, exit(0)
 cmp         dword ptr [r14+01Ch],3
 jne         .L13
  CHECK_TAG R2, tnumber, exit(0)
 cmp         dword ptr [r14+02Ch],3
 jne         .L13
  %4 = LOAD_DOUBLE R1
 vmovsd      xmm0,qword ptr [r14+010h]
  %6 = ADD_NUM %4, R2
 vaddsd      xmm0,xmm0,qword ptr [r14+020h]
  INTERRUPT 0u
 mov         rax,qword ptr [r15+<offset>]
 cmp         qword ptr [rax+<offset>],0
 jne         .L14
.L15:
  STORE_DOUBLE R3, %6
 vmovsd      qword ptr [r14+030h],xmm0
  STORE_TAG R3, tnumber
 mov         dword ptr [r14+03Ch],3
  CHECK_TAG R4, tnumber, bb_exit_1
   ; exit sync: R1, {%6}
 cmp         dword ptr [r14+04Ch],3
 jne         .L16
  %14 = LOAD_DOUBLE R4
 vmovsd      xmm0,qword ptr [r14+040h]
  STORE_DOUBLE R1, %14
 vmovsd      qword ptr [r14+010h],xmm0
  RETURN R1, 3i
 lea         rdi,[r14-010h]
 vmovups     xmm0,xmmword ptr [r14+010h]
 vmovups     xmmword ptr [rdi],xmm0
 vmovups     xmm0,xmmword ptr [r14+020h]
 vmovups     xmmword ptr [rdi+010h],xmm0
 vmovups     xmm0,xmmword ptr [r14+030h]
 vmovups     xmmword ptr [rdi+020h],xmm0
 add         rdi,30h
 mov         ecx,3
 jmp         .L8

)"
    );
}

TEST_CASE_FIXTURE(IrAssemblyFixture, "MultiNumToXSharedSourceStrandsRestore")
{
    IrOp entry = build.block(IrBlockKind::Internal);
    build.beginBlock(entry);

    IrOp d = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(1));
    IrOp i = build.inst(IrCmd::NUM_TO_INT, d);
    IrOp u = build.inst(IrCmd::NUM_TO_UINT, d);

    // Kill R1 as a potential restore location
    IrOp doubled = build.inst(IrCmd::ADD_NUM, d, d);
    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(1), doubled);
    build.inst(IrCmd::STORE_TAG, build.vmReg(1), build.constTag(tnumber));

    build.inst(IrCmd::INTERRUPT, build.constUint(0));

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(2), build.inst(IrCmd::INT_TO_NUM, i));
    build.inst(IrCmd::STORE_TAG, build.vmReg(2), build.constTag(tnumber));

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(3), build.inst(IrCmd::UINT_TO_NUM, u));
    build.inst(IrCmd::STORE_TAG, build.vmReg(3), build.constTag(tnumber));

    build.inst(IrCmd::RETURN, build.vmReg(1), build.constInt(3));
    updateUseCounts(build.function);

    // %1 (eax) and %2 (edx) are preserved across the inline interrupt
    // check; out-of-line emitInterrupt saves/restores all call-clobbered
    // GPRs so no stack spill is needed.
    CHECK_EQ(
        "\n" + lower(),
        R"(
; align 32 using ud2
bb_0:
.L12:
  %0 = LOAD_DOUBLE R1
 vmovsd      xmm0,qword ptr [r14+010h]
  %1 = NUM_TO_INT %0
 vcvttsd2si  eax,xmm0
  %2 = NUM_TO_UINT %0
 vcvttsd2si  rdx,xmm0
  %3 = ADD_NUM %0, %0
 vaddsd      xmm0,xmm0,xmm0
  STORE_DOUBLE R1, %3
 vmovsd      qword ptr [r14+010h],xmm0
  STORE_TAG R1, tnumber
 mov         dword ptr [r14+01Ch],3
  INTERRUPT 0u
 mov         rcx,qword ptr [r15+<offset>]
 cmp         qword ptr [rcx+<offset>],0
 jne         .L13
.L14:
  %7 = INT_TO_NUM %1
 vcvtsi2sd   xmm0,xmm0,eax
  STORE_DOUBLE R2, %7
 vmovsd      qword ptr [r14+020h],xmm0
  STORE_TAG R2, tnumber
 mov         dword ptr [r14+02Ch],3
  %10 = UINT_TO_NUM %2
 mov         eax,edx
 vcvtsi2sd   xmm0,xmm0,rax
  STORE_DOUBLE R3, %10
 vmovsd      qword ptr [r14+030h],xmm0
  STORE_TAG R3, tnumber
 mov         dword ptr [r14+03Ch],3
  RETURN R1, 3i
 lea         rdi,[r14-010h]
 vmovups     xmm0,xmmword ptr [r14+010h]
 vmovups     xmmword ptr [rdi],xmm0
 vmovups     xmm0,xmmword ptr [r14+020h]
 vmovups     xmmword ptr [rdi+010h],xmm0
 vmovups     xmm0,xmmword ptr [r14+030h]
 vmovups     xmmword ptr [rdi+020h],xmm0
 add         rdi,30h
 mov         ecx,3
 jmp         .L8

)"
    );
}

TEST_CASE_FIXTURE(IrAssemblyFixture, "DseHintUpdateRedirectsLazyRestoreToLaterReg")
{
    ScopedFastFlag luauCodegenDseRestoreHints{FFlag::LuauCodegenDseRestoreHints, true};
    ScopedFastFlag luauCodegenDseRestoreHintUpdate{FFlag::LuauCodegenDseRestoreHintUpdate, true};

    IrOp entry = build.block(IrBlockKind::Internal);
    build.beginBlock(entry);

    IrOp d = build.inst(IrCmd::LOAD_DOUBLE, build.vmReg(1));
    IrOp i = build.inst(IrCmd::NUM_TO_INT, d);

    // Kill R1 as a potential non-lazy restore location for 'd'
    IrOp doubled = build.inst(IrCmd::ADD_NUM, d, d);
    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(1), doubled);
    build.inst(IrCmd::STORE_TAG, build.vmReg(1), build.constTag(tnumber));

    IrOp roundtrip = build.inst(IrCmd::INT_TO_NUM, i);

    // Two dead stores in R4 and R5, final lazy restore location should be R5
    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(4), roundtrip);
    build.inst(IrCmd::STORE_TAG, build.vmReg(4), build.constTag(tnumber));
    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(5), roundtrip);
    build.inst(IrCmd::STORE_TAG, build.vmReg(5), build.constTag(tnumber));

    build.inst(IrCmd::INTERRUPT, build.constUint(0));

    build.inst(IrCmd::STORE_DOUBLE, build.vmReg(2), roundtrip);
    build.inst(IrCmd::STORE_TAG, build.vmReg(2), build.constTag(tnumber));

    build.inst(IrCmd::RETURN, build.vmReg(1), build.constInt(2));
    updateUseCounts(build.function);

    // %5 is kept in xmm0 across the inline interrupt fast-path.
    // Out-of-line emitInterrupt saves/restores call-clobbered xmm regs, so
    // no lazy restore spill to R5 is necessary.
    CHECK_EQ(
        "\n" + lower(),
        R"(
; align 32 using ud2
bb_0:
.L12:
  %0 = LOAD_DOUBLE R1
 vmovsd      xmm0,qword ptr [r14+010h]
  %1 = NUM_TO_INT %0
 vcvttsd2si  eax,xmm0
  %2 = ADD_NUM %0, %0
 vaddsd      xmm0,xmm0,xmm0
  STORE_DOUBLE R1, %2
 vmovsd      qword ptr [r14+010h],xmm0
  STORE_TAG R1, tnumber
 mov         dword ptr [r14+01Ch],3
  %5 = INT_TO_NUM %1
 vcvtsi2sd   xmm0,xmm0,eax
  INTERRUPT 0u
 mov         rax,qword ptr [r15+<offset>]
 cmp         qword ptr [rax+<offset>],0
 jne         .L13
.L14:
  STORE_DOUBLE R2, %5
 vmovsd      qword ptr [r14+020h],xmm0
  STORE_TAG R2, tnumber
 mov         dword ptr [r14+02Ch],3
  RETURN R1, 2i
 lea         rdi,[r14-010h]
 vmovups     xmm0,xmmword ptr [r14+010h]
 vmovups     xmmword ptr [rdi],xmm0
 vmovups     xmm0,xmmword ptr [r14+020h]
 vmovups     xmmword ptr [rdi+010h],xmm0
 add         rdi,20h
 mov         ecx,2
 jmp         .L8

)"
    );
}

TEST_SUITE_END();
