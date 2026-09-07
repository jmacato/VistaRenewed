#include "metallib_writer.hpp"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

using namespace llvm;

namespace dxmt::metallib {

template <typename T> std::string_view value(const T &value) {
  return std::string_view((const char *)(&value), sizeof(T));
};

struct InputAttribute {
  uint8_t attribute;
  std::string name;
  uint8_t type;
  // TODO: extend patch/control point here
};

/* Second integer of a metadata tuple like !{i32 2, i32 6, i32 0}. */
static bool
get_version_pair(const llvm::Module &module, StringRef md_name, uint16_t ops[2], unsigned first_op) {
  auto named = module.getNamedMetadata(md_name);
  if (!named || !named->getNumOperands())
    return false;
  auto tuple = named->getOperand(0);
  if (tuple->getNumOperands() < first_op + 2)
    return false;
  for (unsigned i = 0; i < 2; i++) {
    auto cam = dyn_cast<ConstantAsMetadata>(tuple->getOperand(first_op + i).get());
    if (!cam)
      return false;
    auto ci = dyn_cast<ConstantInt>(cam->getValue());
    if (!ci)
      return false;
    ops[i] = (uint16_t)ci->getZExtValue();
  }
  return true;
}

/*
 * The compat rewrites in this file exist for pre-Metal-3.1 AIR readers
 * (macOS < 14, iOS/tvOS < 17): their AGX bitcode readers predate several
 * constructs the LLVM 15 pipeline emits.  A module's declared air.version
 * names the reader it will meet: airconv emits 2.5 exactly when the runtime
 * OS predates Metal 3.1 (SM50_SHADER_METAL_300) and 2.6/2.7 otherwise, so
 * gate on it -- a modern reader accepts everything as-is, and one rewrite
 * (`freeze` removal) is a genuine semantic weakening that must not be
 * applied where it is not needed.
 */
static bool
needsLegacyAIRCompat(const llvm::Module &module) {
  uint16_t pair[2];
  if (get_version_pair(module, "air.version", pair, 0))
    return pair[0] < 2 || (pair[0] == 2 && pair[1] < 6);
  /* no declared version: the container VERS default below says 2.6 */
  return false;
}

/* Replace `poison` with `undef`, recursively through constant aggregates and
 * expressions.  Poison only reached bitcode in LLVM 12 (a new constant record
 * code); the AIR readers in older OSes reject it, and unlike the attribute
 * strip below the failure is deferred -- the metallib loads and the function
 * materializes, then the AGX backend fails every pipeline built from it
 * ("Failed to materializeAll", observed on iOS 16.3).  Undef is strictly more
 * defined than poison, so this is what Apple's older compilers emitted. */
static Constant *
depoison(Constant *C, DenseMap<Constant *, Constant *> &cache) {
  if (isa<PoisonValue>(C))
    return UndefValue::get(C->getType());
  if (!isa<ConstantAggregate>(C) && !isa<ConstantExpr>(C))
    return C;
  auto cached = cache.find(C);
  if (cached != cache.end())
    return cached->second;
  SmallVector<Constant *, 8> ops;
  bool changed = false;
  for (unsigned i = 0; i < C->getNumOperands(); i++) {
    auto *op = cast<Constant>(C->getOperand(i));
    auto *replaced = depoison(op, cache);
    changed |= replaced != op;
    ops.push_back(replaced);
  }
  Constant *R = C;
  if (changed) {
    if (auto *CE = dyn_cast<ConstantExpr>(C))
      R = CE->getWithOperands(ops);
    else if (isa<ConstantVector>(C))
      R = ConstantVector::get(ops);
    else if (auto *CS = dyn_cast<ConstantStruct>(C))
      R = ConstantStruct::get(CS->getType(), ops);
    else if (auto *CA = dyn_cast<ConstantArray>(C))
      R = ConstantArray::get(CA->getType(), ops);
  }
  cache[C] = R;
  return R;
}

/*
 * Drop fragment-input parameters the function never reads.  The converter
 * declares every input in the DXBC signature; the optimizer then removes the
 * dead reads, leaving arguments with no uses.  Older AGX compilers (observed
 * on iOS 16.3) fail every pipeline whose fragment function carries such a
 * dead [[user(...)]] input -- "Failed to materializeAll" with no diagnostic
 * -- while current macOS drivers silently ignore them.  A pruned input is
 * always legal: interstage matching is by name and the vertex side may
 * produce outputs the fragment side does not consume.
 */
static void
pruneUnusedFragmentInputs(Module &module) {
  auto frag_md = module.getNamedMetadata("air.fragment");
  if (!frag_md)
    return;
  auto &context = module.getContext();
  for (unsigned fi = 0; fi < frag_md->getNumOperands(); fi++) {
    MDNode *entry = frag_md->getOperand(fi);
    if (entry->getNumOperands() < 3)
      continue;
    auto *fn_const = dyn_cast_or_null<ConstantAsMetadata>(entry->getOperand(0).get());
    auto *inputs = dyn_cast_or_null<MDTuple>(entry->getOperand(2).get());
    if (!fn_const || !inputs)
      continue;
    auto *F = dyn_cast<Function>(fn_const->getValue());
    if (!F || F->isDeclaration())
      continue;

    /* which arguments go: fragment inputs with no uses */
    SmallVector<bool, 16> drop(F->arg_size(), false);
    bool any = false;
    for (auto &input : inputs->operands()) {
      auto *tuple = dyn_cast<MDTuple>(input.get());
      if (!tuple || tuple->getNumOperands() < 2)
        continue;
      auto *idx_md = dyn_cast_or_null<ConstantAsMetadata>(tuple->getOperand(0).get());
      auto *kind = dyn_cast_or_null<MDString>(tuple->getOperand(1).get());
      if (!idx_md || !kind || kind->getString() != "air.fragment_input")
        continue;
      uint64_t idx = cast<ConstantInt>(idx_md->getValue())->getZExtValue();
      if (idx < F->arg_size() && F->getArg(idx)->use_empty()) {
        drop[idx] = true;
        any = true;
      }
    }
    if (!any)
      continue;

    /* rebuild the function without the dead parameters */
    SmallVector<Type *, 16> param_types;
    for (unsigned i = 0; i < F->arg_size(); i++)
      if (!drop[i])
        param_types.push_back(F->getArg(i)->getType());
    auto *new_type = llvm::FunctionType::get(F->getReturnType(), param_types, false);
    auto *NF = Function::Create(new_type, F->getLinkage(), "", &module);
    NF->takeName(F);
    NF->copyAttributesFrom(F);
    /* copyAttributesFrom copied the whole AttributeList, whose parameter
     * slots are indexed against the old signature; an attribute on a
     * parameter after a pruned one lands on the wrong parameter -- or past
     * the last one, which is verifier-invalid ("Attribute after last
     * parameter!").  Rebuild the list against the surviving parameters. */
    {
      auto old_attrs = F->getAttributes();
      SmallVector<AttributeSet, 16> kept_param_attrs;
      for (unsigned i = 0; i < F->arg_size(); i++)
        if (!drop[i])
          kept_param_attrs.push_back(old_attrs.getParamAttrs(i));
      NF->setAttributes(AttributeList::get(
          context, old_attrs.getFnAttrs(), old_attrs.getRetAttrs(),
          kept_param_attrs));
    }
    NF->getBasicBlockList().splice(NF->begin(), F->getBasicBlockList());
    unsigned new_idx = 0;
    for (unsigned i = 0; i < F->arg_size(); i++) {
      if (drop[i])
        continue;
      auto *old_arg = F->getArg(i);
      auto *new_arg = NF->getArg(new_idx++);
      new_arg->takeName(old_arg);
      old_arg->replaceAllUsesWith(new_arg);
    }

    /* rebuild the input metadata with arguments renumbered */
    SmallVector<Metadata *, 16> new_inputs;
    auto *i32 = Type::getInt32Ty(context);
    new_idx = 0;
    for (auto &input : inputs->operands()) {
      auto *tuple = dyn_cast<MDTuple>(input.get());
      if (!tuple || tuple->getNumOperands() < 2) {
        new_inputs.push_back(input.get());
        continue;
      }
      auto *idx_md = dyn_cast_or_null<ConstantAsMetadata>(tuple->getOperand(0).get());
      if (!idx_md) {
        new_inputs.push_back(input.get());
        continue;
      }
      uint64_t idx = cast<ConstantInt>(idx_md->getValue())->getZExtValue();
      if (idx < drop.size() && drop[idx])
        continue; /* pruned */
      SmallVector<Metadata *, 16> ops(tuple->op_begin(), tuple->op_end());
      ops[0] = ConstantAsMetadata::get(ConstantInt::get(i32, new_idx++));
      new_inputs.push_back(MDTuple::get(context, ops));
    }

    SmallVector<Metadata *, 4> entry_ops(entry->op_begin(), entry->op_end());
    entry_ops[0] = ConstantAsMetadata::get(NF);
    entry_ops[2] = MDTuple::get(context, new_inputs);
    frag_md->setOperand(fi, MDTuple::get(context, entry_ops));

    F->eraseFromParent();
  }
}

/* The iOS 16.3-era AGX bitcode reader predates several constructs the LLVM 15
 * optimization pipeline can introduce even though airconv never emits them
 * directly: min/max/abs intrinsics (InstCombine canonicalizes icmp+select
 * chains into them since LLVM 12), funnel shifts (rotate idioms), saturating
 * arithmetic (clamp idioms), and `freeze` (IPSCCP).  Apple's own AIR
 * toolchain never produces any of these, so lower them back to the classic
 * forms before serialization.  Unknown llvm.* intrinsics outside a known-old
 * allowlist are logged so field captures surface new leaks immediately. */
static void
lowerModernIntrinsics(llvm::Module &module) {
  SmallVector<Instruction *, 8> to_erase;
  for (auto &F : module) {
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *FI = dyn_cast<FreezeInst>(&I)) {
          FI->replaceAllUsesWith(FI->getOperand(0));
          to_erase.push_back(FI);
          continue;
        }
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI)
          continue;
        auto *callee = CI->getCalledFunction();
        if (!callee || !callee->isIntrinsic())
          continue;
        IRBuilder<> b(CI);
        Value *r = nullptr;
        Value *a = CI->arg_size() > 0 ? CI->getArgOperand(0) : nullptr;
        Value *c = CI->arg_size() > 1 ? CI->getArgOperand(1) : nullptr;
        switch (callee->getIntrinsicID()) {
        case Intrinsic::smax:
          r = b.CreateSelect(b.CreateICmpSGT(a, c), a, c);
          break;
        case Intrinsic::smin:
          r = b.CreateSelect(b.CreateICmpSLT(a, c), a, c);
          break;
        case Intrinsic::umax:
          r = b.CreateSelect(b.CreateICmpUGT(a, c), a, c);
          break;
        case Intrinsic::umin:
          r = b.CreateSelect(b.CreateICmpULT(a, c), a, c);
          break;
        case Intrinsic::abs: {
          auto *zero = Constant::getNullValue(a->getType());
          auto *neg = b.CreateSub(zero, a);
          r = b.CreateSelect(b.CreateICmpSLT(a, zero), neg, a);
          break;
        }
        case Intrinsic::fshl:
        case Intrinsic::fshr: {
          /* Branchless legalization: modulo the shift, feed the second
           * operand through an unconditional >>1 so the amount stays in
           * range when the modulo is zero.  The intrinsics are defined as
           * `amt mod bitwidth` for EVERY width; the and/xor forms of the
           * modulo and its complement are only right for powers of two. */
          Value *x = CI->getArgOperand(0), *y = CI->getArgOperand(1),
                *amt = CI->getArgOperand(2);
          auto *ty = cast<IntegerType>(x->getType()->getScalarType());
          unsigned bw = ty->getBitWidth();
          auto *bw1 = ConstantInt::get(amt->getType(), bw - 1);
          Value *cm, *inv;
          if (isPowerOf2_32(bw)) {
            cm = b.CreateAnd(amt, bw1);
            inv = b.CreateXor(cm, bw1);
          } else {
            cm = b.CreateURem(amt, ConstantInt::get(amt->getType(), bw));
            inv = b.CreateSub(bw1, cm);
          }
          if (callee->getIntrinsicID() == Intrinsic::fshl)
            r = b.CreateOr(b.CreateShl(x, cm),
                           b.CreateLShr(b.CreateLShr(y, ConstantInt::get(y->getType(), 1)), inv));
          else
            r = b.CreateOr(b.CreateShl(b.CreateShl(x, ConstantInt::get(x->getType(), 1)), inv),
                           b.CreateLShr(y, cm));
          break;
        }
        case Intrinsic::uadd_sat: {
          auto *sum = b.CreateAdd(a, c);
          r = b.CreateSelect(b.CreateICmpULT(sum, a),
                             Constant::getAllOnesValue(a->getType()), sum);
          break;
        }
        case Intrinsic::usub_sat: {
          auto *diff = b.CreateSub(a, c);
          r = b.CreateSelect(b.CreateICmpULT(a, c),
                             Constant::getNullValue(a->getType()), diff);
          break;
        }
        case Intrinsic::sadd_sat:
        case Intrinsic::ssub_sat: {
          bool is_add = callee->getIntrinsicID() == Intrinsic::sadd_sat;
          auto *sum = is_add ? b.CreateAdd(a, c) : b.CreateSub(a, c);
          /* overflow iff sign(a) != sign(sum) and (add: sign(b) == sign(a);
           * sub: sign(b) != sign(a)) */
          auto *xa = b.CreateXor(a, sum);
          auto *xb = is_add ? b.CreateXor(c, sum) : b.CreateXor(a, c);
          auto *ov = b.CreateICmpSLT(b.CreateAnd(xa, xb),
                                     Constant::getNullValue(a->getType()));
          auto *ty = cast<IntegerType>(a->getType()->getScalarType());
          auto *int_min = ConstantInt::get(
              a->getType(), APInt::getSignedMinValue(ty->getBitWidth()));
          auto *int_max = ConstantInt::get(
              a->getType(), APInt::getSignedMaxValue(ty->getBitWidth()));
          auto *sat = b.CreateSelect(
              b.CreateICmpSLT(a, Constant::getNullValue(a->getType())), int_min,
              int_max);
          r = b.CreateSelect(ov, sat, sum);
          break;
        }
        default:
          break;
        }
        if (r) {
          CI->replaceAllUsesWith(r);
          to_erase.push_back(CI);
        }
      }
    }
  }
  for (auto *I : to_erase)
    I->eraseFromParent();
  /* drop the now-unused declarations: even a dead unknown-intrinsic declare
   * is a record an old reader has no business seeing */
  SmallVector<Function *, 4> dead_decls;
  for (auto &F : module)
    if (F.isDeclaration() && F.use_empty() && F.getName().startswith("llvm."))
      dead_decls.push_back(&F);
  for (auto *F : dead_decls)
    F->eraseFromParent();
  /* surface anything else that could bite an old reader */
  for (auto &F : module) {
    if (!F.isDeclaration() || !F.getName().startswith("llvm."))
      continue;
    static const char *known_old[] = {
        "llvm.uadd.with.overflow", "llvm.usub.with.overflow", "llvm.fabs",
        "llvm.sqrt",  "llvm.floor", "llvm.ceil",  "llvm.trunc",
        "llvm.rint",  "llvm.nearbyint", "llvm.minnum", "llvm.maxnum",
        "llvm.fma",   "llvm.fmuladd",   "llvm.pow",    "llvm.exp2",
        "llvm.log2",  "llvm.bswap",     "llvm.ctpop",  "llvm.ctlz",
        "llvm.cttz",  "llvm.memcpy",    "llvm.memset", "llvm.lifetime",
    };
    bool known = false;
    for (auto *p : known_old)
      if (F.getName().startswith(p)) {
        known = true;
        break;
      }
    if (!known && !F.use_empty())
      fprintf(stderr, "airconv: WARNING: module carries modern intrinsic %s; "
                      "older AIR readers may reject it\n",
              F.getName().str().c_str());
  }
}

void MetallibWriter::Write(llvm::Module &module, raw_ostream &OS) {
  /* Modules for Metal 3.1+ readers (air.version >= 2.6) keep every modern
   * construct: only the attribute strip below applies there, exactly the
   * pre-compat behavior of this writer.  See needsLegacyAIRCompat. */
  const bool legacy_air = needsLegacyAIRCompat(module);
  if (legacy_air) {
    lowerModernIntrinsics(module);
    pruneUnusedFragmentInputs(module);
  }

  SmallVector<char, 0> bitcode;
  SmallVector<char, 0> public_metadata;
  SmallVector<char, 0> private_metadata;
  SmallVector<char, 0> function_def;

  uint32_t fn_count = 0;

  /* The optimization pipeline infers attributes the AIR bitcode readers in
   * older OSes predate; an unknown attribute is a fatal "LLVM ERROR: Unknown
   * attribute kind" inside MTLCompilerService (observed on iOS 16.3, whose
   * reader also predates macOS 14's).  Apple's own tools strip these on
   * serialization -- a metallib built with -sdk iphoneos keeps only the
   * LLVM-8-era set -- so do the same.  Hints only, no semantic loss. */
  AttributeMask incompatible;
  incompatible.addAttribute(Attribute::MustProgress);
  incompatible.addAttribute(Attribute::NoFree);
  incompatible.addAttribute(Attribute::NoSync);
  incompatible.addAttribute(Attribute::WillReturn);
  incompatible.addAttribute(Attribute::NoUndef);
  incompatible.addAttribute(Attribute::NoCallback);
  DenseMap<Constant *, Constant *> depoison_cache;
  for (auto &F : module) {
    F.removeFnAttrs(incompatible);
    F.removeRetAttrs(incompatible);
    for (unsigned i = 0; i < F.arg_size(); i++)
      F.removeParamAttrs(i, incompatible);
    for (auto &BB : F)
      for (auto &I : BB) {
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          CB->removeFnAttrs(incompatible);
          CB->removeRetAttrs(incompatible);
          for (unsigned i = 0; i < CB->arg_size(); i++)
            CB->removeParamAttrs(i, incompatible);
        }
        if (!legacy_air)
          continue;
        for (unsigned i = 0; i < I.getNumOperands(); i++)
          if (auto *C = dyn_cast<Constant>(I.getOperand(i))) {
            auto *replaced = depoison(C, depoison_cache);
            if (replaced != C)
              I.setOperand(i, replaced);
          }
        /* Fast-math flags on a `phi` extend the bitcode PHI record with a
         * trailing flags field (newer LLVM); the PHI record body is
         * (value, block) pairs, so the odd trailing element breaks the old
         * reader's parse and pipeline creation fails materializeAll on
         * iOS 16.3 (seen via the fast-math'd f16 tessellation helpers).
         * Flags on a phi only grant optimization latitude, so dropping
         * them is semantically safe.  The other flag-tail carriers are
         * fine on the same reader (device-verified): fcmp/select append
         * to fixed-arity records it parses positionally, and calls signal
         * flags through an explicit bit -- only phi needs this. */
        if (isa<PHINode>(I) && isa<FPMathOperator>(I) &&
            I.getFastMathFlags().any())
          I.setFast(false); /* setFastMathFlags only ORs; this clears */
        /* iOS 16.3 AGX deterministically fails materializeAll on pipelines
         * whose modules chain commutative FP ops accumulator-first
         * (fadd(acc, new)); identical math written fadd(new, acc) -- what
         * fxc's SM5 backend and Apple's compilers emit -- passes.
         * Canonicalize to the passing form: later-defined operand first. */
        if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
          if (BO->isCommutative()) {
            auto *a = dyn_cast<Instruction>(BO->getOperand(0));
            auto *b = dyn_cast<Instruction>(BO->getOperand(1));
            if (a && b && a->getParent() == b->getParent() &&
                a->comesBefore(b))
              BO->swapOperands();
          }
        }
      }
    if (!legacy_air)
      continue;
    /* `fneg` serializes as FUNC_CODE_INST_UNOP (LLVM 8+); Apple's AIR
     * toolchain still writes the LLVM-5-era `fsub -0.0, x` form and the
     * iOS 16.3 AGX materializer misbehaves on pipelines whose modules carry
     * the UNOP record. Rewrite to the classic form. */
    {
      SmallVector<UnaryOperator *, 4> fnegs;
      for (auto &BB : F)
        for (auto &I : BB)
          if (auto *UO = dyn_cast<UnaryOperator>(&I))
            if (UO->getOpcode() == Instruction::FNeg)
              fnegs.push_back(UO);
      for (auto *UO : fnegs) {
        auto *zero = ConstantFP::getNegativeZero(UO->getType());
        auto *sub = BinaryOperator::CreateFSub(zero, UO->getOperand(0), "", UO);
        sub->copyFastMathFlags(UO);
        UO->replaceAllUsesWith(sub);
        UO->eraseFromParent();
      }
    }
    /* Same bug family: scalar extractelements that linger far from their
     * producer (fail shape) vs sitting right after it (pass shape). Hoist
     * each single-vector extract to just after its producing instruction.
     * A phi producer may not be the last phi of its block, and non-phi
     * instructions must not be spliced into the phi group ("PHI nodes not
     * grouped at top of basic block") -- the closest legal spot there is
     * the block's first insertion point. */
    for (auto &BB : F) {
      SmallVector<ExtractElementInst *, 8> extracts;
      for (auto &I : BB)
        if (auto *EE = dyn_cast<ExtractElementInst>(&I))
          if (isa<Constant>(EE->getIndexOperand()))
            if (auto *src = dyn_cast<Instruction>(EE->getVectorOperand()))
              if (src->getParent() == &BB &&
                  (isa<PHINode>(src) ? &*BB.getFirstInsertionPt() != EE
                                     : src->getNextNode() != EE))
                extracts.push_back(EE);
      for (auto *EE : extracts) {
        auto *src = cast<Instruction>(EE->getVectorOperand());
        if (isa<PHINode>(src))
          EE->moveBefore(BB, BB.getFirstInsertionPt());
        else
          EE->moveAfter(src);
      }
    }
  }
  if (legacy_air)
    for (auto &G : module.globals())
      if (G.hasInitializer()) {
        auto *replaced = depoison(G.getInitializer(), depoison_cache);
        if (replaced != G.getInitializer())
          G.setInitializer(replaced);
      }

  raw_svector_ostream bitcode_stream(bitcode);
  WriteBitcodeToFile(module, bitcode_stream, false, nullptr, true);

  auto hash =
    compute_sha256_hash((const uint8_t *)bitcode.data(), bitcode.size());

  /* The VERS tags have to state what the bitcode actually is: the loader
   * cross-checks them against the module's own air.version. */
  MTLB_VERS_TAG vers{};
  uint16_t pair[2];
  vers.airVersionMajor = 2, vers.airVersionMinor = 6;
  if (get_version_pair(module, "air.version", pair, 0))
    vers.airVersionMajor = pair[0], vers.airVersionMinor = pair[1];
  vers.languageVersionMajor = 3, vers.languageVersionMinor = 1;
  /* air.language_version is !{!"Metal", major, minor, patch} */
  if (get_version_pair(module, "air.language_version", pair, 1))
    vers.languageVersionMajor = pair[0], vers.languageVersionMinor = pair[1];

  raw_svector_ostream public_metadata_stream(public_metadata);
  raw_svector_ostream private_metadata_stream(private_metadata);

  {

    raw_svector_ostream function_def_stream(function_def);

    auto vertexFns = module.getNamedMetadata("air.vertex");
    if (vertexFns) {
      for (auto fn : vertexFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Vertex});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
        while (fn->getNumOperands() > 3 && isa<MDTuple>(fn->getOperand(3).get())
        ) {
          auto maybe_patch_tuple = cast<MDTuple>(fn->getOperand(3).get());
          if (maybe_patch_tuple->getNumOperands() != 4)
            break;
          if (!isa<MDString>(maybe_patch_tuple->getOperand(0).get()))
            break;
          auto air_patch =
            cast<MDString>(maybe_patch_tuple->getOperand(0).get());
          if (air_patch->getString() != "air.patch")
            break;
          if (!isa<MDString>(maybe_patch_tuple->getOperand(1).get()))
            break;
          auto air_patch_value =
            cast<MDString>(maybe_patch_tuple->getOperand(1).get());
          if (air_patch_value->getString() == "triangle") {
            function_def_stream
              << value(MTLB_TESS_TAG{.patchType = 1, .controlPointCount = 0});
          } else {
            function_def_stream
              << value(MTLB_TESS_TAG{.patchType = 2, .controlPointCount = 0});
          }
          break;
        }
        function_def_stream << "ENDT";
        auto inputs = dyn_cast<MDTuple>(fn->getOperand(2).get());

        std::vector<InputAttribute> attributes;

        for (auto &input : inputs->operands()) {
          auto inputElement = dyn_cast<MDTuple>(input.get());
          auto inputKind =
            dyn_cast<MDString>(inputElement->getOperand(1))->getString();
          if (inputKind == "air.vertex_input") {
            uint32_t location =
              dyn_cast<ConstantInt>(
                dyn_cast<ConstantAsMetadata>(inputElement->getOperand(3))
                  ->getValue()
              )
                ->getValue()
                .getZExtValue();
            auto typeName =
              dyn_cast<MDString>(inputElement->getOperand(6))->getString();
            auto argName =
              dyn_cast<MDString>(inputElement->getOperand(8))->getString();
            attributes.push_back(InputAttribute{
              .attribute = (uint8_t)location,
              .name = (argName.str()),
              .type =
                (uint8_t)(typeName == "float4"  ? 0x06
                          : typeName == "uint4" ? 0x24
                                                : 0x20), // REFACTOR IT: hope it
                                                         // works in general?
            });
          }
        }
        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        if (attributes.size()) {
          // if no vertex attributes, then don't emit VATY, otherwise PSO
          // doesn't compile
          fn_public_metadata_stream << value(MTLBFourCC::VertexAttribute);
          auto lenOffset = fn_public_metadata_stream.tell();
          fn_public_metadata_stream << value((uint16_t)0);
          fn_public_metadata_stream << value((uint16_t)attributes.size());
          for (auto &vattr : attributes) {
            fn_public_metadata_stream << vattr.name << '\0';
            fn_public_metadata_stream << value(MTLB_VATY{
              .attribute = vattr.attribute,
              .__ = 0,
              .usage = 0,
              .active = 1,
            });
          }
          auto vatt_written = fn_public_metadata_stream.tell() - lenOffset;
          *(uint16_t *)(&fn_public_metadata[lenOffset]) = vatt_written - 2;
          fn_public_metadata_stream << value(MTLBFourCC::VertexAttributeType);
          fn_public_metadata_stream << value((uint16_t)(2 + attributes.size()));
          fn_public_metadata_stream << value((uint16_t)(attributes.size()));
          for (auto &vattr : attributes) {
            fn_public_metadata_stream << value(vattr.type);
          }
        }
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
    auto fragmentFns = module.getNamedMetadata("air.fragment");
    if (fragmentFns) {
      for (auto fn : fragmentFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Fragment});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
        function_def_stream << "ENDT";
        public_metadata_stream << value(4);
        public_metadata_stream << "ENDT";
        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
    auto kernelFns = module.getNamedMetadata("air.kernel");
    if (kernelFns) {
      for (auto fn : kernelFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Kernel});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
        function_def_stream << "ENDT";

        std::vector<InputAttribute> attributes;

        // auto inputs = dyn_cast<MDTuple>(fn->getOperand(2).get());
        // for (auto &input : inputs->operands()) {
        //   auto inputElement = dyn_cast<MDTuple>(input.get());
        //   auto inputKind =
        //     dyn_cast<MDString>(inputElement->getOperand(1))->getString();
        //   if (inputKind == "air.vertex_input") {
        //     uint32_t location =
        //       dyn_cast<ConstantInt>(
        //         dyn_cast<ConstantAsMetadata>(inputElement->getOperand(3))
        //           ->getValue()
        //       )
        //         ->getValue()
        //         .getZExtValue();
        //     auto typeName =
        //       dyn_cast<MDString>(inputElement->getOperand(6))->getString();
        //     auto argName =
        //       dyn_cast<MDString>(inputElement->getOperand(8))->getString();
        //     attributes.push_back(InputAttribute{
        //       .attribute = (uint8_t)location,
        //       .name = (argName.str()),
        //       .type =
        //         (uint8_t)(typeName == "float4"  ? 0x06
        //                   : typeName == "uint4" ? 0x24
        //                                         : 0x20), // REFACTOR IT: hope
        //                                         it
        //                                                  // works in general?
        //     });
        //   }
        // }
        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        // fn_public_metadata_stream << value(MTLBFourCC::VertexAttribute);
        // auto lenOffset = fn_public_metadata_stream.tell();
        // fn_public_metadata_stream << value((uint16_t)0);
        // fn_public_metadata_stream << value((uint16_t)attributes.size());
        // for (auto &vattr : attributes) {
        //   fn_public_metadata_stream << vattr.name << '\0';
        //   fn_public_metadata_stream << value(MTLB_VATY{
        //     .attribute = vattr.attribute,
        //     .__ = 0,
        //     .usage = 0,
        //     .active = 1,
        //   });
        // }
        // auto vatt_written = fn_public_metadata_stream.tell() - lenOffset;
        // *(uint16_t *)(&fn_public_metadata[lenOffset]) = vatt_written - 2;
        // fn_public_metadata_stream << value(MTLBFourCC::VertexAttributeType);
        // fn_public_metadata_stream << value((uint16_t)(2 +
        // attributes.size())); fn_public_metadata_stream <<
        // value((uint16_t)(attributes.size())); for (auto &vattr : attributes)
        // {
        //   fn_public_metadata_stream << value(vattr.type);
        // }
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }

    auto objectFns = module.getNamedMetadata("air.object");
    if (objectFns) {
      for (auto fn : objectFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Object});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
        function_def_stream << "ENDT";

        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }

    auto meshFns = module.getNamedMetadata("air.mesh");
    if (meshFns) {
      for (auto fn : meshFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Mesh});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
        function_def_stream << "ENDT";

        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
  }

  MTLBHeader header;
  header.Magic = MTLB_Magic;
  header.FileSize =
    sizeof(MTLBHeader) + sizeof(uint32_t) /* fn count */ +
    sizeof(uint32_t) /* constant: function list size */ + function_def.size() +
    sizeof(MTLBFourCC::EndTag) /* extended header*/
    + public_metadata.size() + private_metadata.size() + bitcode.size();
  header.FunctionListOffset = sizeof(MTLBHeader);
  header.FunctionListSize = function_def.size() + 4;
  header.PublicMetadataOffset =
    header.FunctionListOffset + header.FunctionListSize +
    sizeof(uint32_t) // extra room for function count
    + sizeof(MTLBFourCC::EndTag);
  header.PublicMetadataSize = public_metadata.size();
  header.PrivateMetadataOffset =
    header.PublicMetadataOffset + header.PublicMetadataSize;
  header.PrivateMetadataSize = private_metadata.size();
  header.BitcodeOffset =
    header.PrivateMetadataOffset + header.PrivateMetadataSize;
  header.BitcodeSize = bitcode.size();

  header.Type = FileType::MTLBType_Executable; // executable
  header.VersionMajor = 2;
  header.VersionMinor = 7;
  /* Like the AIR triple, the container names the platform it will be loaded
   * on, so it follows the platform this library was built for (a Wine cross
   * build has no TARGET_OS_* and falls through to macOS).  A mismatch is
   * "This library format is not supported on this platform" at load. */
#if defined(TARGET_OS_VISION) && TARGET_OS_VISION
  header.Platform = Platform::MTLBPlatform_Embedded;
  header.OS = TARGET_OS_SIMULATOR ? OS::MTLBOS_visionOSSimulator : OS::MTLBOS_visionOS;
  header.OSVersionMajor = 1;
  header.OSVersionMinor = 0;
#elif defined(TARGET_OS_TV) && TARGET_OS_TV
  header.Platform = Platform::MTLBPlatform_Embedded;
  header.OS = TARGET_OS_SIMULATOR ? OS::MTLBOS_tvOSSimulator : OS::MTLBOS_tvOS;
  header.OSVersionMajor = 16;
  header.OSVersionMinor = 0;
#elif defined(TARGET_OS_IOS) && TARGET_OS_IOS
  header.Platform = Platform::MTLBPlatform_Embedded;
  header.OS = TARGET_OS_SIMULATOR ? OS::MTLBOS_iOSSimulator : OS::MTLBOS_iOS;
  header.OSVersionMajor = 16;
  header.OSVersionMinor = 0;
#else
  header.Platform = Platform::MTLBPlatform_macOS;
  header.OS = OS::MTLBOS_macOS;
  header.OSVersionMajor = 14;
  header.OSVersionMinor = 4;
#endif
  // write to stream
  OS << value(header);
  OS << value(fn_count);
  OS << value((uint32_t)header.FunctionListSize);
  OS << function_def;
  OS.write("ENDT", 4); // extend header
  OS << public_metadata;
  OS << private_metadata;
  OS << bitcode;
}

} // namespace dxmt::metallib