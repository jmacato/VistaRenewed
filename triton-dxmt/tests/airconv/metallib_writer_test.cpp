/* Unit tests for the old-AIR-reader compatibility transforms in
 * MetallibWriter::Write: constructs modules containing the constructs the
 * LLVM 15 pipeline can produce but pre-LLVM-8-era AIR bitcode readers
 * (e.g. iOS 16.3's AGX driver) reject -- fneg / FUNC_CODE_INST_UNOP,
 * poison constants, and modern integer intrinsics (min/max/abs, funnel
 * shifts, saturating arithmetic), runs them through the writer, reparses
 * the emitted bitcode, and asserts both that the constructs are gone and
 * that the replacement expansions compute the right values. */

#include "metallib_writer.hpp"

#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cinttypes>
#include <cstdio>
#include <string>

using namespace llvm;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    g_checks++;                                                                \
    if (!(cond)) {                                                             \
      g_failures++;                                                            \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                     \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static const char *kModulePrologue =
    "target datalayout = \"e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-"
    "i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-"
    "v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-"
    "v512:512:512-v1024:1024:1024-n8:16:32\"\n"
    "target triple = \"air64-apple-ios16.0.0\"\n";

/* The compat transforms only apply to modules whose declared AIR target is
 * consumed by a pre-Metal-3.1 reader (air.version < 2.6); the tests declare
 * it explicitly the way airconv does. */
static const char *kLegacyAirVersion =
    "!air.version = !{!9990}\n"
    "!9990 = !{i32 2, i32 5, i32 0}\n";
static const char *kModernAirVersion =
    "!air.version = !{!9990}\n"
    "!9990 = !{i32 2, i32 6, i32 0}\n";

/* Round-trip a module through MetallibWriter::Write and reparse the bitcode
 * region declared by the container header.  Whatever the writer emits must
 * be a structurally valid module -- an invalid one is exactly the
 * undiagnosable "Failed to materializeAll" this pass exists to avoid. */
static std::unique_ptr<Module>
roundtrip(Module &m, LLVMContext &ctx) {
  SmallVector<char, 0> out;
  raw_svector_ostream os(out);
  dxmt::metallib::MetallibWriter writer;
  writer.Write(m, os);
  if (out.size() < sizeof(dxmt::metallib::MTLBHeader))
    return nullptr;
  auto *hdr = reinterpret_cast<const dxmt::metallib::MTLBHeader *>(out.data());
  if (hdr->BitcodeOffset + hdr->BitcodeSize > out.size())
    return nullptr;
  StringRef bc(out.data() + hdr->BitcodeOffset, hdr->BitcodeSize);
  auto buf = MemoryBuffer::getMemBufferCopy(bc, "roundtrip");
  auto modOrErr = parseBitcodeFile(buf->getMemBufferRef(), ctx);
  if (!modOrErr) {
    consumeError(modOrErr.takeError());
    return nullptr;
  }
  auto mod = std::move(modOrErr.get());
  std::string verr;
  raw_string_ostream verrs(verr);
  if (verifyModule(*mod, &verrs)) {
    fprintf(stderr, "emitted module fails the verifier: %s\n",
            verrs.str().c_str());
    return nullptr;
  }
  return mod;
}

static std::unique_ptr<Module>
parseIR(LLVMContext &ctx, const std::string &body, bool legacy_target = true) {
  SMDiagnostic err;
  auto m = parseAssemblyString(
      kModulePrologue + body +
          (legacy_target ? kLegacyAirVersion : kModernAirVersion),
      err, ctx);
  if (!m) {
    fprintf(stderr, "test IR parse error: %s\n", err.getMessage().str().c_str());
    g_failures++;
  }
  return m;
}

/* Evaluate a straight-line, argument-free function returning an integer by
 * constant-folding each instruction in order. */
static bool
evaluate(Function *f, uint64_t &result) {
  const DataLayout &dl = f->getParent()->getDataLayout();
  DenseMap<Value *, Constant *> env;
  auto resolve = [&](Value *v) -> Constant * {
    if (auto *c = dyn_cast<Constant>(v))
      return c;
    auto it = env.find(v);
    return it == env.end() ? nullptr : it->second;
  };
  for (auto &I : instructions(*f)) {
    if (auto *ret = dyn_cast<ReturnInst>(&I)) {
      auto *c = resolve(ret->getReturnValue());
      if (!c)
        return false;
      if (auto *ci = dyn_cast<ConstantInt>(c)) {
        result = ci->getZExtValue();
        return true;
      }
      return false;
    }
    SmallVector<Constant *, 4> ops;
    for (auto &op : I.operands()) {
      auto *c = resolve(op.get());
      if (!c)
        return false;
      ops.push_back(c);
    }
    Constant *folded = ConstantFoldInstOperands(&I, ops, dl);
    if (!folded && isa<SelectInst>(I))
      folded = ConstantExpr::getSelect(ops[0], ops[1], ops[2]);
    if (!folded)
      return false;
    env[&I] = folded;
  }
  return false;
}

static void
test_fneg_rewrite() {
  LLVMContext ctx;
  auto m = parseIR(ctx, R"(
define float @f(float %x) {
  %n = fneg fast float %x
  ret float %n
}
define <4 x float> @g(<4 x float> %x) {
  %n = fneg <4 x float> %x
  ret <4 x float> %n
}
)");
  if (!m)
    return;
  auto rt = roundtrip(*m, ctx);
  CHECK(rt != nullptr, "fneg module did not roundtrip");
  if (!rt)
    return;
  for (auto &F : *rt)
    for (auto &I : instructions(F))
      CHECK(!isa<UnaryOperator>(I), "fneg (UNOP) survived serialization");
  /* the replacement must be the classic fsub -0.0, x with flags kept */
  auto *f = rt->getFunction("f");
  bool found = false;
  for (auto &I : instructions(*f)) {
    if (auto *bo = dyn_cast<BinaryOperator>(&I)) {
      CHECK(bo->getOpcode() == Instruction::FSub, "expected fsub replacement");
      auto *lhs = dyn_cast<ConstantFP>(bo->getOperand(0));
      CHECK(lhs && lhs->isNegative() && lhs->isZero(),
            "fsub lhs is not -0.0");
      CHECK(bo->getFastMathFlags().isFast(), "fast-math flags dropped");
      found = true;
    }
  }
  CHECK(found, "no fsub replacement emitted");
}

static void
test_poison_depoison() {
  LLVMContext ctx;
  auto m = parseIR(ctx, R"(
define <4 x float> @f(<4 x float> %x) {
  %r = fmul <4 x float> %x, <float 2.5e-1, float poison, float poison, float poison>
  ret <4 x float> %r
}
)");
  if (!m)
    return;
  auto rt = roundtrip(*m, ctx);
  CHECK(rt != nullptr, "poison module did not roundtrip");
  if (!rt)
    return;
  for (auto &F : *rt)
    for (auto &I : instructions(F))
      for (auto &op : I.operands()) {
        if (auto *c = dyn_cast<Constant>(op.get()))
          CHECK(!c->containsPoisonElement(), "poison constant survived");
      }
}

static void
test_phi_fmf_stripped() {
  LLVMContext ctx;
  auto m = parseIR(ctx, R"(
define half @f(i1 %c, half %a, half %b) {
entry:
  br i1 %c, label %t, label %e
t:
  %x = fmul fast half %a, %b
  br label %e
e:
  %p = phi fast half [ %x, %t ], [ %a, %entry ]
  ret half %p
}
)");
  if (!m)
    return;
  auto rt = roundtrip(*m, ctx);
  CHECK(rt != nullptr, "phi-fmf module did not roundtrip");
  if (!rt)
    return;
  for (auto &F : *rt)
    for (auto &I : instructions(F)) {
      if (isa<PHINode>(I))
        CHECK(!I.getFastMathFlags().any(), "fast-math flags survived on phi");
      if (I.getOpcode() == Instruction::FMul)
        CHECK(I.getFastMathFlags().isFast(), "fmul flags were wrongly stripped");
    }
}

static void
test_freeze_removed() {
  LLVMContext ctx;
  auto m = parseIR(ctx, R"(
define i32 @f(i32 %x) {
  %fr = freeze i32 %x
  %r = add i32 %fr, 1
  ret i32 %r
}
)");
  if (!m)
    return;
  auto rt = roundtrip(*m, ctx);
  CHECK(rt != nullptr, "freeze module did not roundtrip");
  if (!rt)
    return;
  for (auto &F : *rt)
    for (auto &I : instructions(F))
      CHECK(!isa<FreezeInst>(I), "freeze survived serialization");
}

struct IntrinsicCase {
  const char *expr; /* body computing i32/i64 into %r */
  uint64_t expected;
};

static void
test_intrinsic_expansion() {
  /* golden values, including the edge cases the expansions must honor */
  static const IntrinsicCase cases[] = {
      {"%r = call i32 @llvm.smax.i32(i32 5, i32 -3)", 5},
      {"%r = call i32 @llvm.smin.i32(i32 5, i32 -3)", 0xFFFFFFFD},
      {"%r = call i32 @llvm.umax.i32(i32 1, i32 -1)", 0xFFFFFFFF},
      {"%r = call i32 @llvm.umin.i32(i32 1, i32 -1)", 1},
      {"%r = call i32 @llvm.abs.i32(i32 -7, i1 false)", 7},
      /* abs(INT_MIN, poison=false) wraps to INT_MIN */
      {"%r = call i32 @llvm.abs.i32(i32 -2147483648, i1 false)", 0x80000000},
      {"%r = call i32 @llvm.fshl.i32(i32 305419896, i32 -1698898192, i32 4)",
       0x23456789}, /* fshl(0x12345678, 0x9ABCDEF0, 4) */
      {"%r = call i32 @llvm.fshl.i32(i32 305419896, i32 -1698898192, i32 0)",
       0x12345678}, /* shift of zero returns first arg */
      {"%r = call i32 @llvm.fshl.i32(i32 305419896, i32 -1698898192, i32 32)",
       0x12345678}, /* shift amount is modulo bitwidth */
      {"%r = call i32 @llvm.fshr.i32(i32 305419896, i32 -1698898192, i32 4)",
       0x89ABCDEF},
      {"%r = call i32 @llvm.fshr.i32(i32 305419896, i32 -1698898192, i32 0)",
       0x9ABCDEF0}, /* fshr by zero returns second arg */
      {"%r = call i32 @llvm.uadd.sat.i32(i32 -16, i32 32)", 0xFFFFFFFF},
      {"%r = call i32 @llvm.uadd.sat.i32(i32 3, i32 4)", 7},
      {"%r = call i32 @llvm.usub.sat.i32(i32 16, i32 32)", 0},
      {"%r = call i32 @llvm.usub.sat.i32(i32 32, i32 16)", 16},
      {"%r = call i32 @llvm.sadd.sat.i32(i32 2147483632, i32 32)", 0x7FFFFFFF},
      {"%r = call i32 @llvm.sadd.sat.i32(i32 -2147483632, i32 -32)",
       0x80000000},
      {"%r = call i32 @llvm.sadd.sat.i32(i32 3, i32 4)", 7},
      {"%r = call i32 @llvm.ssub.sat.i32(i32 -2147483632, i32 32)",
       0x80000000},
      {"%r = call i32 @llvm.ssub.sat.i32(i32 2147483632, i32 -32)",
       0x7FFFFFFF},
      {"%r = call i32 @llvm.ssub.sat.i32(i32 7, i32 3)", 4},
  };
  static const char *kDecls =
      "declare i32 @llvm.smax.i32(i32, i32)\n"
      "declare i32 @llvm.smin.i32(i32, i32)\n"
      "declare i32 @llvm.umax.i32(i32, i32)\n"
      "declare i32 @llvm.umin.i32(i32, i32)\n"
      "declare i32 @llvm.abs.i32(i32, i1)\n"
      "declare i32 @llvm.fshl.i32(i32, i32, i32)\n"
      "declare i32 @llvm.fshr.i32(i32, i32, i32)\n"
      "declare i32 @llvm.uadd.sat.i32(i32, i32)\n"
      "declare i32 @llvm.usub.sat.i32(i32, i32)\n"
      "declare i32 @llvm.sadd.sat.i32(i32, i32)\n"
      "declare i32 @llvm.ssub.sat.i32(i32, i32)\n";
  for (auto &c : cases) {
    LLVMContext ctx;
    auto m = parseIR(ctx, std::string("define i32 @f() {\n  ") + c.expr +
                              "\n  ret i32 %r\n}\n" + kDecls);
    if (!m)
      continue;
    auto rt = roundtrip(*m, ctx);
    CHECK(rt != nullptr, "did not roundtrip: %s", c.expr);
    if (!rt)
      continue;
    /* no modern intrinsic may survive (a call would reparse as one) */
    for (auto &F : *rt)
      CHECK(!(F.isDeclaration() && F.getName().startswith("llvm.")),
            "intrinsic declaration survived: %s (%s)",
            F.getName().str().c_str(), c.expr);
    uint64_t got = ~0ull;
    bool ok = evaluate(rt->getFunction("f"), got);
    CHECK(ok, "expansion not evaluable: %s", c.expr);
    if (ok)
      CHECK(got == c.expected, "%s: got 0x%" PRIx64 " want 0x%" PRIx64,
            c.expr, got, c.expected);
  }
  /* vector variant sanity: expansion applies element-wise */
  {
    LLVMContext ctx;
    auto m = parseIR(ctx, R"(
define <2 x i32> @f(<2 x i32> %a, <2 x i32> %b) {
  %r = call <2 x i32> @llvm.smax.v2i32(<2 x i32> %a, <2 x i32> %b)
  ret <2 x i32> %r
}
declare <2 x i32> @llvm.smax.v2i32(<2 x i32>, <2 x i32>)
)");
    if (m) {
      auto rt = roundtrip(*m, ctx);
      CHECK(rt != nullptr, "vector smax did not roundtrip");
      if (rt)
        for (auto &F : *rt)
          CHECK(!(F.isDeclaration() && F.getName().startswith("llvm.")),
                "vector smax survived");
    }
  }
}

/* A vector phi followed by another phi, with the extractelement further down
 * the block: the hoist must not splice the extract into the middle of the
 * phi group ("PHI nodes not grouped at top of basic block"). */
static void
test_extract_hoist_phi_group() {
  LLVMContext ctx;
  auto m = parseIR(ctx, R"(
define float @f(i1 %c, <4 x float> %v, <4 x float> %w) {
entry:
  br label %loop
loop:
  %acc = phi <4 x float> [ %v, %entry ], [ %next, %loop ]
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %next = fadd <4 x float> %acc, %w
  %inc = add i32 %i, 1
  %done = icmp eq i32 %inc, 4
  %x = extractelement <4 x float> %acc, i32 0
  br i1 %done, label %exit, label %loop
exit:
  %r = phi float [ %x, %loop ]
  ret float %r
}
)");
  if (!m)
    return;
  auto rt = roundtrip(*m, ctx);
  CHECK(rt != nullptr, "phi-group extract module did not roundtrip validly");
  if (!rt)
    return;
  /* every phi must still sit at the top of its block */
  for (auto &F : *rt)
    for (auto &BB : F) {
      bool seen_non_phi = false;
      for (auto &I : BB) {
        if (!isa<PHINode>(I))
          seen_non_phi = true;
        else
          CHECK(!seen_non_phi, "phi found after a non-phi instruction");
      }
    }
}

/* Hoisting must still happen for non-phi producers (that is the compat
 * transform's whole point). */
static void
test_extract_hoist_still_hoists() {
  LLVMContext ctx;
  auto m = parseIR(ctx, R"(
define float @f(<4 x float> %v, <4 x float> %w) {
  %sum = fadd <4 x float> %v, %w
  %unrelated = fsub <4 x float> %v, %w
  %also = fmul <4 x float> %unrelated, %w
  %x = extractelement <4 x float> %sum, i32 1
  %y = extractelement <4 x float> %also, i32 2
  %r = fadd float %x, %y
  ret float %r
}
)");
  if (!m)
    return;
  auto rt = roundtrip(*m, ctx);
  CHECK(rt != nullptr, "extract hoist module did not roundtrip");
  if (!rt)
    return;
  auto *f = rt->getFunction("f");
  for (auto &I : instructions(*f))
    if (auto *EE = dyn_cast<ExtractElementInst>(&I)) {
      auto *src = cast<Instruction>(EE->getVectorOperand());
      CHECK(src->getNextNode() == EE,
            "extractelement was not hoisted next to its producer");
    }
}

/* Pruning a dead fragment input must rebuild the parameter attribute list
 * against the new signature: an attribute stuck at its old index either
 * lands on the wrong parameter or, past the last parameter, fails the
 * verifier ("Attribute after last parameter!"). */
static void
test_pruned_param_attrs() {
  LLVMContext ctx;
  auto m = parseIR(ctx, R"IR(
define float @f(float %dead, float* nocapture readonly %buf) {
  %v = load float, float* %buf
  ret float %v
}
!air.fragment = !{!8000}
!8000 = !{float (float, float*)* @f, !"air.frag", !8001}
!8001 = !{!8002}
!8002 = !{i32 0, !"air.fragment_input", !"user(locn0)"}
)IR");
  if (!m)
    return;
  auto rt = roundtrip(*m, ctx);
  CHECK(rt != nullptr, "pruned-attr module did not roundtrip validly");
  if (!rt)
    return;
  auto *f = rt->getFunction("f");
  CHECK(f != nullptr, "pruned function lost its name");
  if (!f)
    return;
  CHECK(f->arg_size() == 1, "dead fragment input was not pruned (%u args)",
        (unsigned)f->arg_size());
  if (f->arg_size() != 1)
    return;
  CHECK(f->hasParamAttribute(0, Attribute::NoCapture),
        "nocapture lost or misplaced after pruning");
  CHECK(f->hasParamAttribute(0, Attribute::ReadOnly),
        "readonly lost or misplaced after pruning");
}

/* llvm.fshl/fshr are defined as `amt mod bitwidth` for every width; the
 * mask-based lowering is only correct for powers of two. */
static void
test_fshl_non_pow2_width() {
  static const IntrinsicCase cases[] = {
      /* fshl.i24(0x123456, 0xABCDEF, 8) = (x<<8 | y>>16) & 0xFFFFFF */
      {"%r = call i24 @llvm.fshl.i24(i24 1193046, i24 11259375, i24 8)",
       0x3456AB},
      {"%r = call i24 @llvm.fshl.i24(i24 1193046, i24 11259375, i24 0)",
       0x123456},
      /* shift == bitwidth wraps to zero */
      {"%r = call i24 @llvm.fshl.i24(i24 1193046, i24 11259375, i24 24)",
       0x123456},
      /* fshr.i24(x, y, 8) = (x<<16 | y>>8) & 0xFFFFFF */
      {"%r = call i24 @llvm.fshr.i24(i24 1193046, i24 11259375, i24 8)",
       0x56ABCD},
      {"%r = call i24 @llvm.fshr.i24(i24 1193046, i24 11259375, i24 24)",
       0xABCDEF},
  };
  for (auto &c : cases) {
    LLVMContext ctx;
    auto m = parseIR(ctx, std::string("define i24 @f() {\n  ") + c.expr +
                              "\n  ret i24 %r\n}\n"
                              "declare i24 @llvm.fshl.i24(i24, i24, i24)\n"
                              "declare i24 @llvm.fshr.i24(i24, i24, i24)\n");
    if (!m)
      continue;
    auto rt = roundtrip(*m, ctx);
    CHECK(rt != nullptr, "did not roundtrip: %s", c.expr);
    if (!rt)
      continue;
    uint64_t got = ~0ull;
    bool ok = evaluate(rt->getFunction("f"), got);
    CHECK(ok, "expansion not evaluable: %s", c.expr);
    if (ok)
      CHECK(got == c.expected, "%s: got 0x%" PRIx64 " want 0x%" PRIx64, c.expr,
            got, c.expected);
  }
}

/* Modules targeting AIR 2.6+ (Metal 3.1, macOS 14 / iOS 17) are consumed by
 * readers that accept every modern construct; they must pass through with
 * none of the compat rewrites -- in particular `freeze` (whose removal is a
 * genuine semantic weakening) must survive. */
static void
test_modern_target_untouched() {
  LLVMContext ctx;
  auto m = parseIR(ctx, R"(
define float @f(float %x, i32 %i, i1 %c) {
entry:
  %n = fneg fast float %x
  %fr = freeze i32 %i
  %s = call i32 @llvm.smax.i32(i32 %fr, i32 4)
  br i1 %c, label %t, label %e
t:
  br label %e
e:
  %p = phi fast float [ %n, %t ], [ %x, %entry ]
  %pi = sitofp i32 %s to float
  %r = fadd float %p, %pi
  ret float %r
}
define <4 x float> @g(<4 x float> %x) {
  %r = fmul <4 x float> %x, <float 2.5e-1, float poison, float poison, float poison>
  ret <4 x float> %r
}
declare i32 @llvm.smax.i32(i32, i32)
)",
                   /*legacy_target=*/false);
  if (!m)
    return;
  auto rt = roundtrip(*m, ctx);
  CHECK(rt != nullptr, "modern-target module did not roundtrip");
  if (!rt)
    return;
  bool has_fneg = false, has_freeze = false, has_phi_fmf = false;
  for (auto &F : *rt)
    for (auto &I : instructions(F)) {
      if (auto *UO = dyn_cast<UnaryOperator>(&I))
        has_fneg |= UO->getOpcode() == Instruction::FNeg;
      has_freeze |= isa<FreezeInst>(I);
      if (isa<PHINode>(I))
        has_phi_fmf |= I.getFastMathFlags().any();
    }
  CHECK(has_fneg, "fneg was rewritten on a modern target");
  CHECK(has_freeze, "freeze was removed on a modern target");
  CHECK(has_phi_fmf, "phi fast-math flags were stripped on a modern target");
  CHECK(rt->getFunction("llvm.smax.i32") != nullptr,
        "llvm.smax was lowered on a modern target");
  bool has_poison = false;
  for (auto &F : *rt)
    for (auto &I : instructions(F))
      for (auto &op : I.operands())
        if (auto *cc = dyn_cast<Constant>(op.get()))
          has_poison |= cc->containsPoisonElement();
  CHECK(has_poison, "poison was rewritten on a modern target");
}

static void
test_ancient_intrinsics_kept() {
  /* the allowlisted old intrinsics must pass through untouched */
  LLVMContext ctx;
  auto m = parseIR(ctx, R"(
define { i32, i1 } @f(i32 %a, i32 %b) {
  %r = call { i32, i1 } @llvm.uadd.with.overflow.i32(i32 %a, i32 %b)
  ret { i32, i1 } %r
}
declare { i32, i1 } @llvm.uadd.with.overflow.i32(i32, i32)
)");
  if (!m)
    return;
  auto rt = roundtrip(*m, ctx);
  CHECK(rt != nullptr, "uadd.with.overflow module did not roundtrip");
  if (!rt)
    return;
  CHECK(rt->getFunction("llvm.uadd.with.overflow.i32") != nullptr,
        "ancient intrinsic was incorrectly removed");
}

int
main() {
  test_fneg_rewrite();
  test_poison_depoison();
  test_freeze_removed();
  test_phi_fmf_stripped();
  test_intrinsic_expansion();
  test_ancient_intrinsics_kept();
  test_extract_hoist_phi_group();
  test_extract_hoist_still_hoists();
  test_pruned_param_attrs();
  test_fshl_non_pow2_width();
  test_modern_target_untouched();
  fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
