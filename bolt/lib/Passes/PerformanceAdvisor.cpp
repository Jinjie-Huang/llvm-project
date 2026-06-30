//===- bolt/Passes/PerformanceAdvisor.cpp ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/PerformanceAdvisor.h"
#include "bolt/Core/MCInstUtils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "bolt-performance-advisor"

namespace llvm {
namespace bolt {

namespace {

struct HotSpillReport {
  MCInstReference Location;
  uint64_t ExecutionCount{0};
  MCPhysReg SrcReg{0};
  uint16_t StackPtrReg{0};
  int64_t StackOffset{0};
  uint8_t Size{0};
};

template <typename T> static void iterateOverInstrs(BinaryFunction &BF, T Fn) {
  if (BF.hasCFG()) {
    for (BinaryBasicBlock &BB : BF)
      for (int64_t I = 0, E = BB.size(); I < E; ++I)
        Fn(MCInstReference(BB, I));
  } else {
    for (auto I = BF.instrs().begin(), E = BF.instrs().end(); I != E; ++I)
      Fn(MCInstReference(BF, I));
  }
}

static uint64_t getProfileCountForLocation(const MCInstReference &Location) {
  if (const BinaryBasicBlock *BB = Location.getBasicBlock())
    return BB->getKnownExecutionCount();
  const BinaryFunction *BF = Location.getFunction();
  return std::max(BF->getKnownExecutionCount(), BF->getRawSampleCount());
}

static std::string formatStackSlot(const BinaryContext &BC,
                                   uint16_t StackPtrReg,
                                   int64_t StackOffset) {
  std::string Slot;
  raw_string_ostream OS(Slot);
  OS << '[';
  if (StackPtrReg)
    OS << BC.MRI->getName(StackPtrReg);
  else
    OS << "unknown-base";
  if (StackOffset < 0)
    OS << " - " << -StackOffset;
  else if (StackOffset > 0)
    OS << " + " << StackOffset;
  OS << ']';
  return OS.str();
}

static void printHotSpillReport(raw_ostream &OS, const BinaryContext &BC,
                                const HotSpillReport &Report) {
  const BinaryFunction *BF = Report.Location.getFunction();
  const BinaryBasicBlock *BB = Report.Location.getBasicBlock();
  const uint64_t Address = Report.Location.computeAddress();

  OS << "\nPERF-ADVISOR: hot stack spill in function " << BF->getPrintName();
  if (BB)
    OS << ", basic block " << BB->getName();
  OS << ", at address " << llvm::format("%x", Address) << "\n";

  OS << "  The instruction is ";
  BC.printInstruction(OS, Report.Location, Address, BF);

  OS << "  Profile: containing block/function execution count is "
     << Report.ExecutionCount << "; function entry count is "
     << BF->getKnownExecutionCount() << ".\n";
  OS << "  Spill: stores " << BC.MRI->getName(Report.SrcReg) << " to "
     << formatStackSlot(BC, Report.StackPtrReg, Report.StackOffset) << " ("
     << static_cast<unsigned>(Report.Size) << " bytes).\n";
  OS << "  Hint: this hot register-to-stack store is a likely spill/reload "
        "cost. It often comes from high register pressure or a source value "
        "whose lifetime is too long. Consider reducing live ranges near this "
        "code, splitting complex expressions, avoiding unnecessary address-"
        "taken locals, enabling stronger optimization, or restructuring the "
        "source so the value can stay in a register. If the store is required "
        "by ABI/debuggability (for example frame-pointer-heavy builds), check "
        "whether those constraints are intentional on this hot path.\n";
}

static std::optional<HotSpillReport>
analyzeHotSpill(const BinaryContext &BC, const MCInstReference &Location,
                uint64_t HotThreshold) {
  const uint64_t ExecutionCount = getProfileCountForLocation(Location);
  if (ExecutionCount < HotThreshold)
    return std::nullopt;

  const MCInst &Inst = Location.getMCInst();
  bool IsLoad = false;
  bool IsStore = false;
  bool IsStoreFromReg = false;
  MCPhysReg Reg = BC.MIB->getNoRegister();
  int32_t SrcImm = 0;
  uint16_t StackPtrReg = 0;
  int64_t StackOffset = 0;
  uint8_t Size = 0;
  bool IsSimple = false;
  bool IsIndexed = false;

  if (!BC.MIB->isStackAccess(Inst, IsLoad, IsStore, IsStoreFromReg, Reg, SrcImm,
                             StackPtrReg, StackOffset, Size, IsSimple,
                             IsIndexed))
    return std::nullopt;

  // The first advisor focuses on simple register spills such as
  //   mov %rax, -0x10(%rbp)
  // rather than pushes, read-modify-write stack operations, immediate stores or
  // indexed addressing patterns.
  if (!IsStore || IsLoad || !IsStoreFromReg || !IsSimple || IsIndexed ||
      Reg == BC.MIB->getNoRegister() || BC.MIB->isPush(Inst))
    return std::nullopt;

  return HotSpillReport{Location, ExecutionCount, Reg, StackPtrReg, StackOffset,
                        Size};
}

} // namespace

PerformanceAdvisor::PerformanceAdvisor(opts::GadgetKindBitmask EnabledScanners)
    : BinaryFunctionPass(false), EnabledScanners(EnabledScanners) {
  assert(!(EnabledScanners & ~opts::GS_PERF_ALL_MASK) &&
         "Unrelated scanners requested");
}

Error PerformanceAdvisor::runOnFunctions(BinaryContext &BC) {
  if (!BC.isX86())
    return Error::success();

  if (!(EnabledScanners & opts::GS_PERF_HOT_SPILLS))
    return Error::success();

  const uint64_t HotThreshold = std::max<uint64_t>(BC.getHotThreshold(), 1);
  std::vector<HotSpillReport> Reports;

  for (BinaryFunction *BF : BC.getAllBinaryFunctions()) {
    if (!BF || BF->isIgnored())
      continue;

    iterateOverInstrs(*BF, [&](MCInstReference InstRef) {
      if (std::optional<HotSpillReport> Report =
              analyzeHotSpill(BC, InstRef, HotThreshold))
        Reports.push_back(*Report);
    });
  }

  llvm::sort(Reports, [](const HotSpillReport &A, const HotSpillReport &B) {
    if (A.ExecutionCount != B.ExecutionCount)
      return A.ExecutionCount > B.ExecutionCount;
    return A.Location.computeAddress() < B.Location.computeAddress();
  });

  for (const HotSpillReport &Report : Reports)
    printHotSpillReport(BC.outs(), BC, Report);

  return Error::success();
}

} // namespace bolt
} // namespace llvm
