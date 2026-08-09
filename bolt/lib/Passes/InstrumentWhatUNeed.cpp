//===- bolt/Passes/InstrumentWhatUNeed.cpp --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/InstrumentWhatUNeed.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinaryData.h"
#include "bolt/Core/BinaryFunction.h"
#include "bolt/Core/MCPlusBuilder.h"
#include "bolt/RuntimeLibs/InstrumentWhatUNeedRuntimeLibrary.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

namespace llvm {
namespace bolt {

namespace {

BinaryBasicBlock::iterator
insertInstructions(BinaryBasicBlock &BB, BinaryBasicBlock::iterator Pos,
                   const InstructionListType &Instructions) {
  for (const MCInst &Inst : Instructions) {
    MCInst Copy = Inst;
    Pos = BB.insertInstruction(Pos, std::move(Copy));
    ++Pos;
  }
  return Pos;
}

BinaryBasicBlock::iterator getEntryInsertionPoint(BinaryContext &BC,
                                                  BinaryBasicBlock &BB) {
  auto Pos = BB.begin();
  while (Pos != BB.end() && BC.MIB->isPseudo(*Pos))
    ++Pos;

  if (Pos == BB.end())
    return Pos;

  if (BC.isX86() && BC.MIB->isTerminateBranch(*Pos))
    return std::next(Pos);

  if (BC.isAArch64() && (BC.MIB->isBTILandingPad(*Pos, BTIKind::C) ||
                         BC.MIB->isBTILandingPad(*Pos, BTIKind::J) ||
                         BC.MIB->isBTILandingPad(*Pos, BTIKind::JC)))
    return std::next(Pos);

  return Pos;
}

} // namespace

Error InstrumentWhatUNeed::runOnFunctions(BinaryContext &BC) {
  if (!BC.isELF() || (!BC.isX86() && !BC.isAArch64()))
    return createFatalBOLTError(
        "BOLT-ERROR: --instrument=func-probe supports ELF x86-64 and "
        "AArch64 only\n");

  BinaryData *HookData =
      BC.getBinaryDataByName(opts::InstrumentFuncProbeFunction);
  DenseSet<const BinaryFunction *> HookFunctions;
  MCSymbol *CallTarget = nullptr;
  const bool ExternalHook = InstrumentWhatUNeedRuntimeLibrary::needsRuntime(BC);
  if (HookData) {
    BinaryFunction *HookFunction =
        BC.getFunctionForSymbol(HookData->getSymbol());
    if (HookFunction) {
      CallTarget = HookData->getSymbol();
      SmallVector<BinaryFunction *> HookWorklist{HookFunction};
      while (!HookWorklist.empty()) {
        BinaryFunction *Function = HookWorklist.pop_back_val();
        if (!HookFunctions.insert(Function).second)
          continue;
        llvm::append_range(HookWorklist, Function->getFragments());
        if (Function->isFragment())
          llvm::append_range(HookWorklist, *Function->getParentFragments());
      }
    }
  }

  if (!CallTarget) {
    if (!BC.getRuntimeLibrary())
      return createFatalBOLTError(
          "BOLT-ERROR: external func-probe hook requires the "
          "resolver runtime\n");
    BinaryFunction *Dispatch =
        BC.createInjectedBinaryFunction("__bolt_iwyn_local_dispatch");
    BinaryBasicBlock *BB = Dispatch->addBasicBlock();
    MCSymbol *Slot = BC.Ctx->getOrCreateSymbol(
        InstrumentWhatUNeedRuntimeLibrary::DispatchOffsetName);
    InstructionListType Instructions =
        BC.MIB->createInstrumentedFunctionDispatch(Slot, BC.Ctx.get());
    BB->addInstructions(Instructions.begin(), Instructions.end());
    BB->setCFIState(0);
    Dispatch->updateState(BinaryFunction::State::CFG_Finalized);
    CallTarget = Dispatch->getSymbol();
  }

  const InstructionListType Call =
      BC.MIB->createInstrumentedFunctionCall(CallTarget, BC.Ctx.get());

  uint64_t InstrumentedFunctions = 0;
  uint64_t EntryCalls = 0;
  uint64_t ExitCalls = 0;
  uint64_t SkippedFunctions = 0;

  for (auto &BFI : BC.getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;
    if (HookFunctions.contains(&Function) || Function.isPseudo() ||
        Function.isIgnored() || Function.isFolded())
      continue;

    if (!Function.hasCFG() || (!BC.HasRelocations && !Function.isSimple())) {
      ++SkippedFunctions;
      if (opts::Verbosity >= 1)
        BC.errs() << "BOLT-WARNING: cannot move " << Function
                  << " without input relocations; skipping "
                     "--instrument=func-probe\n";
      continue;
    }

    uint64_t FunctionEntries = 0;
    uint64_t FunctionExits = 0;
    for (BinaryBasicBlock &BB : Function) {
      // A split fragment is part of its parent invocation, not a new one.
      const bool IsProcessEntry =
          ExternalHook && BC.StartFunctionAddress &&
          Function.getAddress() == *BC.StartFunctionAddress;
      if (!Function.isFragment() && !IsProcessEntry && BB.isEntryPoint()) {
        insertInstructions(BB, getEntryInsertionPoint(BC, BB), Call);
        ++FunctionEntries;
      }

      for (auto II = BB.begin(); II != BB.end(); ++II) {
        if (!BC.MIB->isReturn(*II) && !BC.MIB->isTailCall(*II))
          continue;

        II = insertInstructions(BB, II, Call);
        ++FunctionExits;
      }
    }

    if (!FunctionEntries && !FunctionExits)
      continue;

    Function.setNeedsPatch(true);
    if (!BC.HasRelocations)
      Function.setMoveToNewAddress();
    ++InstrumentedFunctions;
    EntryCalls += FunctionEntries;
    ExitCalls += FunctionExits;
  }

  BC.outs() << "BOLT-INFO: func-probe inserted " << EntryCalls
            << " entry call(s) and " << ExitCalls << " exit call(s) in "
            << InstrumentedFunctions << " function(s)";
  if (SkippedFunctions)
    BC.outs() << "; skipped " << SkippedFunctions << " unsupported function(s)";
  BC.outs() << '\n';

  return Error::success();
}

} // namespace bolt
} // namespace llvm
