//===- bolt/Passes/PerformanceAdvisor.cpp ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/PerformanceAdvisor.h"
#include "bolt/Core/DebugData.h"
#include "bolt/Core/MCInstUtils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "llvm/DebugInfo/DWARF/DWARFObject.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "bolt-performance-advisor"

namespace llvm {
namespace bolt {

namespace {

static cl::opt<std::string> PerfAdvisorReportFilename(
    "perf-advisor-report",
    cl::desc("write profile-correlated performance advisor report to file"),
    cl::init("perf-advisor-report.txt"), cl::cat(opts::BinaryAnalysisCategory));

struct HotSpillReport {
  MCInstReference Location;
  MCInstReference ReloadLocation;
  uint64_t ExecutionCount{0};
  MCPhysReg SrcReg{0};
  MCPhysReg ReloadReg{0};
  uint16_t StackPtrReg{0};
  int64_t StackOffset{0};
  uint8_t Size{0};
};

struct StackAccessInfo {
  bool IsLoad{false};
  bool IsStore{false};
  bool IsStoreFromReg{false};
  MCPhysReg Reg{0};
  uint16_t StackPtrReg{0};
  int64_t StackOffset{0};
  uint8_t Size{0};
};

struct LoopStats {
  unsigned InstructionCount{0};
  unsigned BodyBlockCount{0};
  unsigned ExitEdgeCount{0};
  unsigned ConditionalBranchCount{0};
  unsigned CallCount{0};
  unsigned LoadCount{0};
  unsigned StoreCount{0};
  unsigned VectorInstructionCount{0};
  unsigned RegisterCount{0};
  bool HasEarlyExit{false};
};

struct HotLoopReport {
  BinaryFunction *Function{nullptr};
  BinaryBasicBlock *Header{nullptr};
  BinaryBasicBlock *Latch{nullptr};
  uint64_t HeaderExecutionCount{0};
  uint64_t BackedgeCount{0};
  uint64_t LoopHeatCount{0};
  uint64_t EstimatedTripCount{0};
  bool HasBackedgeProfile{false};
  LoopStats Stats;
  bool SuggestUnroll{false};
  bool SuggestVectorize{false};
  bool LikelyAliasOrDependenceBlocker{false};
  bool LikelyTripCountBlocker{false};
  bool LikelyControlFlowBlocker{false};
};

struct FunctionAdviceReport {
  BinaryFunction *Function{nullptr};
  uint64_t Heat{0};
  SmallVector<const HotSpillReport *, 4> Spills;
  SmallVector<const HotLoopReport *, 4> Loops;
};

static std::optional<StackAccessInfo>
getSimpleStackAccess(const BinaryContext &BC, const MCInst &Inst);

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
                                   uint16_t StackPtrReg, int64_t StackOffset) {
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

static bool isVectorInstruction(const BinaryContext &BC, const MCInst &Inst) {
  StringRef OpcodeName = BC.MII->getName(Inst.getOpcode());
  if (OpcodeName.starts_with("V") || OpcodeName.contains("YMM") ||
      OpcodeName.contains("ZMM"))
    return true;

  for (const MCOperand &Operand : Inst) {
    if (!Operand.isReg() || Operand.getReg() == BC.MIB->getNoRegister())
      continue;
    StringRef RegName = BC.MRI->getName(Operand.getReg());
    if (RegName.starts_with("YMM") || RegName.starts_with("ZMM"))
      return true;
  }

  return false;
}

static bool isArithmeticLikeInstruction(const BinaryContext &BC,
                                        const MCInst &Inst) {
  StringRef OpcodeName = BC.MII->getName(Inst.getOpcode());
  return OpcodeName.contains("ADD") || OpcodeName.contains("SUB") ||
         OpcodeName.contains("MUL") || OpcodeName.contains("DIV") ||
         OpcodeName.contains("AND") || OpcodeName.contains("OR") ||
         OpcodeName.contains("XOR") || OpcodeName.contains("SHL") ||
         OpcodeName.contains("SHR") || OpcodeName.contains("SAR") ||
         OpcodeName.contains("INC") || OpcodeName.contains("DEC") ||
         OpcodeName.contains("CMP");
}

static bool isBlockInLoop(ArrayRef<BinaryBasicBlock *> LoopBlocks,
                          const BinaryBasicBlock *BB) {
  return llvm::is_contained(LoopBlocks, BB);
}

static uint64_t getEdgeCount(const BinaryBasicBlock &From,
                             const BinaryBasicBlock &To) {
  return From.getBranchInfo(To).Count;
}

static LoopStats collectLoopStats(const BinaryContext &BC,
                                  ArrayRef<BinaryBasicBlock *> LoopBlocks,
                                  const BinaryBasicBlock *Header,
                                  const BinaryBasicBlock *Latch) {
  LoopStats Stats;
  Stats.BodyBlockCount = LoopBlocks.size();
  SmallSet<MCPhysReg, 32> TouchedRegisters;

  for (const BinaryBasicBlock *BB : LoopBlocks) {
    for (const MCInst &Inst : *BB) {
      if (BC.MIB->isPseudo(Inst))
        continue;

      ++Stats.InstructionCount;
      if (BC.MIB->isConditionalBranch(Inst))
        ++Stats.ConditionalBranchCount;
      if (BC.MIB->isCall(Inst))
        ++Stats.CallCount;
      if (isVectorInstruction(BC, Inst))
        ++Stats.VectorInstructionCount;

      const MCInstrDesc &Desc = BC.MII->get(Inst.getOpcode());
      Stats.LoadCount += Desc.mayLoad();
      Stats.StoreCount += Desc.mayStore();

      for (const MCOperand &Operand : Inst)
        if (Operand.isReg() && Operand.getReg() != BC.MIB->getNoRegister())
          TouchedRegisters.insert(Operand.getReg());
    }

    for (BinaryBasicBlock *Succ : BB->successors()) {
      if (isBlockInLoop(LoopBlocks, Succ))
        continue;
      ++Stats.ExitEdgeCount;
      if (BB != Latch)
        Stats.HasEarlyExit = true;
    }
  }

  Stats.RegisterCount = TouchedRegisters.size();
  // A header with multiple exits usually corresponds to an internal guard or
  // data-dependent early break in the original loop, even if the CFG is
  // compact.
  if (Stats.ExitEdgeCount > 1 && Header != Latch)
    Stats.HasEarlyExit = true;
  return Stats;
}

static std::optional<HotLoopReport> analyzeHotLoop(BinaryFunction &BF,
                                                   BinaryBasicBlock &Latch,
                                                   BinaryBasicBlock &Header,
                                                   uint64_t HotThreshold) {
  if (!BF.hasCFG() || Latch.getLayoutIndex() < Header.getLayoutIndex())
    return std::nullopt;

  const uint64_t BackedgeCount = getEdgeCount(Latch, Header);
  const uint64_t HeaderCount = Header.getKnownExecutionCount();
  const uint64_t LoopHeatCount = std::max(BackedgeCount, HeaderCount);
  const bool HasBackedgeProfile = BackedgeCount > 0;
  if (LoopHeatCount < HotThreshold)
    return std::nullopt;

  SmallVector<BinaryBasicBlock *, 8> LoopBlocks;
  for (BinaryBasicBlock *BB : BF.getLayout().blocks()) {
    if (BB->getLayoutIndex() < Header.getLayoutIndex() ||
        BB->getLayoutIndex() > Latch.getLayoutIndex())
      continue;
    LoopBlocks.push_back(BB);
  }

  if (LoopBlocks.empty())
    return std::nullopt;

  const BinaryContext &BC = BF.getBinaryContext();
  LoopStats Stats = collectLoopStats(BC, LoopBlocks, &Header, &Latch);
  const uint64_t EstimatedTripCount =
      HasBackedgeProfile && HeaderCount ? (BackedgeCount / HeaderCount) + 1 : 0;

  HotLoopReport Report;
  Report.Function = &BF;
  Report.Header = &Header;
  Report.Latch = &Latch;
  Report.HeaderExecutionCount = HeaderCount;
  Report.BackedgeCount = BackedgeCount;
  Report.LoopHeatCount = LoopHeatCount;
  Report.EstimatedTripCount = EstimatedTripCount;
  Report.HasBackedgeProfile = HasBackedgeProfile;
  Report.Stats = Stats;

  const bool SmallEnoughToUnroll = Stats.InstructionCount <= 32;
  const bool RegisterPressureOK = Stats.RegisterCount <= 24;
  const bool NotObviouslyUnrolled = LoopHeatCount >= HotThreshold;
  Report.SuggestUnroll = SmallEnoughToUnroll && !Stats.HasEarlyExit &&
                         Stats.CallCount == 0 && RegisterPressureOK &&
                         NotObviouslyUnrolled;

  const bool HasMemoryTraffic = Stats.LoadCount + Stats.StoreCount >= 2;
  const bool HasCompute = llvm::any_of(LoopBlocks, [&](BinaryBasicBlock *BB) {
    return llvm::any_of(*BB, [&](const MCInst &Inst) {
      return isArithmeticLikeInstruction(BC, Inst);
    });
  });
  const bool ScalarHotLoop = Stats.VectorInstructionCount == 0;
  Report.SuggestVectorize =
      ScalarHotLoop && HasMemoryTraffic && HasCompute && Stats.CallCount == 0 &&
      (LoopHeatCount >= HotThreshold || EstimatedTripCount >= 8);

  Report.LikelyAliasOrDependenceBlocker =
      Report.SuggestVectorize && Stats.LoadCount > 0 && Stats.StoreCount > 0;
  Report.LikelyTripCountBlocker =
      Report.SuggestVectorize &&
      (EstimatedTripCount >= 16 || !HasBackedgeProfile ||
       LoopHeatCount >= std::max<uint64_t>(HotThreshold * 8, 1000));
  Report.LikelyControlFlowBlocker =
      Report.SuggestVectorize &&
      (Stats.HasEarlyExit || Stats.ConditionalBranchCount > 1);

  if (!Report.SuggestUnroll && !Report.SuggestVectorize)
    return std::nullopt;
  return Report;
}

static std::optional<std::string> getSourceLocation(const BinaryContext &BC,
                                                    const MCInst &Inst,
                                                    uint64_t Address) {
  if (!BC.DwCtx)
    return std::nullopt;

  auto FormatLine = [](StringRef FileName, uint32_t Line, uint32_t Column,
                       uint32_t Discriminator =
                           0) -> std::optional<std::string> {
    if (FileName.empty() || Line == 0)
      return std::nullopt;
    std::string Loc;
    raw_string_ostream OS(Loc);
    OS << FileName << ':' << Line;
    if (Column)
      OS << ':' << Column;
    if (Discriminator)
      OS << " discriminator:" << Discriminator;
    return OS.str();
  };

  auto LookupLineTable = [&](const DWARFDebugLine::LineTable &LineTable)
      -> std::optional<std::string> {
    uint32_t RowIndex = LineTable.lookupAddress(
        {Address, object::SectionedAddress::UndefSection});
    if (RowIndex == LineTable.UnknownRowIndex)
      return std::nullopt;

    const DWARFDebugLine::Row &Row = LineTable.Rows[RowIndex];
    std::optional<const char *> FileName =
        dwarf::toString(LineTable.Prologue.getFileNameEntry(Row.File).Name);
    if (!FileName)
      return std::nullopt;
    return FormatLine(*FileName, Row.Line, Row.Column, Row.Discriminator);
  };

  const ClusteredRows *LineTableRows = ClusteredRows::fromSMLoc(Inst.getLoc());
  if (!LineTableRows || LineTableRows->getRows().empty()) {
    for (const std::unique_ptr<DWARFUnit> &Unit : BC.DwCtx->compile_units()) {
      const DWARFDebugLine::LineTable *LineTable =
          BC.DwCtx->getLineTableForUnit(Unit.get());
      if (!LineTable)
        continue;

      if (std::optional<std::string> Loc = LookupLineTable(*LineTable))
        return Loc;
    }

    // Hand-written assembly tests, and some stripped binaries, can contain a
    // usable .debug_line section without any .debug_info compile units. In that
    // case getLineTableForUnit()/getLineInfoForAddress() have nothing to anchor
    // on, so parse the raw line section directly and search every table.
    const DWARFObject &DObj = BC.DwCtx->getDWARFObj();
    if (!DObj.getLineSection().Data.empty()) {
      DWARFDataExtractor LineData(DObj, DObj.getLineSection(),
                                  BC.DwCtx->isLittleEndian(), 0);
      DWARFDebugLine::SectionParser Parser(LineData, *BC.DwCtx,
                                           BC.DwCtx->normal_units());
      while (!Parser.done()) {
        bool UnrecoverableError = false;
        DWARFDebugLine::LineTable LineTable =
            Parser.parseNext([](Error E) { consumeError(std::move(E)); },
                             [&](Error E) {
                               UnrecoverableError = true;
                               consumeError(std::move(E));
                             });
        if (UnrecoverableError)
          break;
        if (std::optional<std::string> Loc = LookupLineTable(LineTable))
          return Loc;
      }
    }

    std::optional<DILineInfo> LineInfo =
        BC.DwCtx->getLineInfoForAddress(object::SectionedAddress{
            Address, object::SectionedAddress::UndefSection});
    if (!LineInfo || LineInfo->FileName == DILineInfo::BadString)
      return std::nullopt;
    return FormatLine(LineInfo->FileName, LineInfo->Line, LineInfo->Column);
  }

  const DebugLineTableRowRef RowRef = LineTableRows->getRows().front();
  DWARFUnit *Unit =
      BC.DwCtx->getCompileUnitForOffset(RowRef.DwCompileUnitIndex);
  if (!Unit)
    return std::nullopt;

  const DWARFDebugLine::LineTable *LineTable =
      BC.DwCtx->getLineTableForUnit(Unit);
  if (!LineTable || RowRef.RowIndex == 0 ||
      RowRef.RowIndex > LineTable->Rows.size())
    return std::nullopt;

  const DWARFDebugLine::Row &Row = LineTable->Rows[RowRef.RowIndex - 1];
  std::optional<const char *> FileName =
      dwarf::toString(LineTable->Prologue.getFileNameEntry(Row.File).Name);
  if (!FileName)
    return std::nullopt;
  return FormatLine(*FileName, Row.Line, Row.Column, Row.Discriminator);
}

static std::optional<std::string>
getHotSpillSourceLocation(const BinaryContext &BC,
                          const HotSpillReport &Report) {
  return getSourceLocation(BC, Report.Location.getMCInst(),
                           Report.Location.computeAddress());
}

static std::optional<std::string>
getHotLoopSourceLocation(const BinaryContext &BC, const HotLoopReport &Report) {
  if (Report.Header->empty())
    return std::nullopt;
  const uint64_t HeaderInputAddress =
      Report.Function->getAddress() + Report.Header->getInputOffset();
  return getSourceLocation(BC, Report.Header->front(), HeaderInputAddress);
}

static void printHotSpillAdvice(raw_ostream &OS, const BinaryContext &BC,
                                const HotSpillReport &Report, unsigned Index) {
  const BinaryFunction *BF = Report.Location.getFunction();
  const BinaryBasicBlock *BB = Report.Location.getBasicBlock();
  const uint64_t Address = Report.Location.computeAddress();
  const uint64_t ReloadAddress = Report.ReloadLocation.computeAddress();

  OS << "  [" << Index << "] Hot stack slot traffic";
  if (BB)
    OS << ", basic block " << BB->getName();
  OS << ", at address " << llvm::format("%x", Address) << "\n";
  if (std::optional<std::string> SourceLoc =
          getHotSpillSourceLocation(BC, Report))
    OS << "      Source: " << *SourceLoc << "\n";

  OS << "      Instruction: ";
  BC.printInstruction(OS, Report.Location, Address, BF);
  OS << "      Heat: containing block/function execution count is "
     << Report.ExecutionCount << "; function entry count is "
     << BF->getKnownExecutionCount() << ".\n";
  OS << "      Stack traffic: stores " << BC.MRI->getName(Report.SrcReg)
     << " to " << formatStackSlot(BC, Report.StackPtrReg, Report.StackOffset)
     << " (" << static_cast<unsigned>(Report.Size) << " bytes).\n";
  OS << "      Evidence: the same stack slot is later reloaded into "
     << BC.MRI->getName(Report.ReloadReg) << " at address "
     << llvm::format("%x", ReloadAddress) << ": ";
  BC.printInstruction(OS, Report.ReloadLocation, ReloadAddress, BF);
  if (std::optional<std::string> SourceLoc = getSourceLocation(
          BC, Report.ReloadLocation.getMCInst(), ReloadAddress))
    OS << "      Reload source: " << *SourceLoc << "\n";
  OS << "      Hint: this is a spill-like hot stack slot, not a proof that the "
        "compiler spilled a value. RBP/RSP-relative slots can also be real "
        "locals, and such locals are still optimization candidates when they "
        "do not need a stable memory address. Check whether the source-level "
        "value is address-taken/escaped, volatile/atomic, ABI/debug home "
        "storage, or alloca-backed memory. If not, reducing register pressure, "
        "shortening live ranges, splitting complex expressions, enabling "
        "stronger optimization, or restructuring the code may keep the value "
        "in a register and remove this hot stack traffic.\n";
}

static void printHotLoopAdvice(raw_ostream &OS, const BinaryContext &BC,
                               const HotLoopReport &Report, unsigned Index) {
  OS << "  [" << Index << "] Hot loop, header " << Report.Header->getName()
     << ", latch " << Report.Latch->getName() << "\n";
  if (std::optional<std::string> SourceLoc =
          getHotLoopSourceLocation(BC, Report))
    OS << "      Source: " << *SourceLoc << "\n";
  OS << "      Heat: header execution count is " << Report.HeaderExecutionCount
     << "; backedge count is " << Report.BackedgeCount
     << "; loop heat count is " << Report.LoopHeatCount;
  if (Report.HasBackedgeProfile)
    OS << "; estimated trip count is " << Report.EstimatedTripCount << ".\n";
  else
    OS << "; estimated trip count is unknown because the profile has no "
          "recorded loop backedge samples.\n";
  OS << "      Loop shape: " << Report.Stats.BodyBlockCount << " block(s), "
     << Report.Stats.InstructionCount << " instruction(s), "
     << Report.Stats.ExitEdgeCount << " exit edge(s), "
     << Report.Stats.LoadCount << " load(s), " << Report.Stats.StoreCount
     << " store(s), " << Report.Stats.RegisterCount
     << " touched register(s).\n";

  if (Report.SuggestUnroll) {
    OS << "      Unroll hint: this is a very hot, compact loop with no obvious "
          "early exit, no calls, and moderate register pressure. The binary "
          "does not look obviously unrolled, so consider source-level "
          "unrolling, `#pragma clang loop unroll(enable)`, or tuning "
          "`-mllvm -unroll-threshold`/profile-guided options for this hot "
          "path. Because the observed trip count is high, the advisor uses a "
          "more aggressive threshold than the compiler's default static "
          "heuristic.\n";
  }

  if (Report.SuggestVectorize) {
    OS << "      Vectorization hint: this hot scalar loop has memory traffic "
          "and "
          "regular arithmetic but no obvious SIMD/vector instructions. Check "
          "why the compiler kept it scalar. ";
    if (Report.LikelyAliasOrDependenceBlocker)
      OS << "A likely blocker is pointer aliasing, memory overlap, or "
            "cross-iteration memory dependence; if the source semantics allow "
            "it, add `__restrict__`, alignment assumptions, or split arrays to "
            "make independence explicit. ";
    if (Report.LikelyControlFlowBlocker)
      OS << "The loop also has internal control flow/early exits; consider "
            "if-conversion, peeling the rare exit, or separating the hot "
            "straight-line path. ";
    if (Report.LikelyTripCountBlocker)
      OS << "The observed loop heat is high"
         << (Report.HasBackedgeProfile ? ", and the observed backedge/trip "
                                         "count is high; "
                                       : "; the exact trip count is unknown "
                                         "because branch-edge samples are not "
                                         "available; ")
         << "if the compiler believed the trip count was small or unknown, "
            "pass PGO, add `#pragma clang loop vectorize(enable)`, or expose "
            "the minimum trip count in the source. ";
    OS << "Use optimization remarks (`-Rpass-missed=loop-vectorize`) on the "
          "source build to confirm the exact compiler-side reason.\n";
  }
}

static void printFunctionAdviceReport(raw_ostream &OS, const BinaryContext &BC,
                                      ArrayRef<FunctionAdviceReport> Reports) {
  if (Reports.empty())
    return;

  OS << "\nPERF-ADVISOR REPORT\n";
  OS << "===================\n";
  OS << "Functions are sorted by hottest advisor evidence. Source locations "
        "point to the instruction/loop header that triggered the hint.\n";

  unsigned FunctionIndex = 1;
  for (const FunctionAdviceReport &FunctionReport : Reports) {
    const BinaryFunction *BF = FunctionReport.Function;
    OS << "\n#" << FunctionIndex++ << " Function: " << BF->getPrintName()
       << "\n";
    OS << "  Function heat: " << FunctionReport.Heat
       << "; function entry count: " << BF->getKnownExecutionCount()
       << "; raw sample count: " << BF->getRawSampleCount() << "\n";
    OS << "  Hint count: "
       << FunctionReport.Spills.size() + FunctionReport.Loops.size()
       << " (spills: " << FunctionReport.Spills.size()
       << ", loops: " << FunctionReport.Loops.size() << ")\n";

    unsigned HintIndex = 1;
    for (const HotLoopReport *Loop : FunctionReport.Loops)
      printHotLoopAdvice(OS, BC, *Loop, HintIndex++);
    for (const HotSpillReport *Spill : FunctionReport.Spills)
      printHotSpillAdvice(OS, BC, *Spill, HintIndex++);
  }
}

static std::optional<StackAccessInfo>
getSimpleStackAccess(const BinaryContext &BC, const MCInst &Inst) {
  StackAccessInfo Info;
  int32_t SrcImm = 0;
  bool IsSimple = false;
  bool IsIndexed = false;

  if (!BC.MIB->isStackAccess(Inst, Info.IsLoad, Info.IsStore,
                             Info.IsStoreFromReg, Info.Reg, SrcImm,
                             Info.StackPtrReg, Info.StackOffset, Info.Size,
                             IsSimple, IsIndexed))
    return std::nullopt;

  if (!IsSimple || IsIndexed || Info.Reg == BC.MIB->getNoRegister())
    return std::nullopt;

  return Info;
}

static bool isMatchingReload(const StackAccessInfo &Store,
                             const StackAccessInfo &Load) {
  return Load.IsLoad && !Load.IsStore &&
         Load.StackPtrReg == Store.StackPtrReg &&
         Load.StackOffset == Store.StackOffset && Load.Size == Store.Size;
}

static std::optional<std::pair<MCInstReference, MCPhysReg>>
findReloadAfterStore(BinaryFunction &BF, const MCInstReference &StoreLocation,
                     const StackAccessInfo &Store) {
  const BinaryContext &BC = BF.getBinaryContext();
  bool SeenStore = false;
  bool Stop = false;
  std::optional<std::pair<MCInstReference, MCPhysReg>> Reload;

  iterateOverInstrs(BF, [&](MCInstReference InstRef) {
    if (Reload || Stop)
      return;
    if (!SeenStore) {
      SeenStore = InstRef == StoreLocation;
      return;
    }

    std::optional<StackAccessInfo> Access =
        getSimpleStackAccess(BC, InstRef.getMCInst());
    if (!Access)
      return;
    if (Access->IsStore && Access->StackPtrReg == Store.StackPtrReg &&
        Access->StackOffset == Store.StackOffset &&
        Access->Size == Store.Size) {
      Stop = true;
      return;
    }
    if (isMatchingReload(Store, *Access))
      Reload = std::make_pair(InstRef, Access->Reg);
  });

  return Reload;
}

static std::optional<HotSpillReport>
analyzeHotSpill(BinaryFunction &BF, const MCInstReference &Location,
                uint64_t HotThreshold) {
  const BinaryContext &BC = BF.getBinaryContext();
  const uint64_t ExecutionCount = getProfileCountForLocation(Location);
  if (ExecutionCount < HotThreshold)
    return std::nullopt;

  const MCInst &Inst = Location.getMCInst();
  std::optional<StackAccessInfo> Access = getSimpleStackAccess(BC, Inst);
  if (!Access)
    return std::nullopt;

  // The first advisor focuses on simple register spills such as
  //   mov %rax, -0x10(%rbp)
  // rather than pushes, read-modify-write stack operations, immediate stores or
  // indexed addressing patterns. A store alone is not enough evidence: require
  // a later reload from the same stack slot to avoid flagging every hot local.
  if (!Access->IsStore || Access->IsLoad || !Access->IsStoreFromReg ||
      BC.MIB->isPush(Inst))
    return std::nullopt;

  std::optional<std::pair<MCInstReference, MCPhysReg>> Reload =
      findReloadAfterStore(BF, Location, *Access);
  if (!Reload)
    return std::nullopt;

  return HotSpillReport{
      Location,       Reload->first,       ExecutionCount,      Access->Reg,
      Reload->second, Access->StackPtrReg, Access->StackOffset, Access->Size};
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

  const bool RunHotSpillScanner = EnabledScanners & opts::GS_PERF_HOT_SPILLS;
  const bool RunHotLoopScanner = EnabledScanners & opts::GS_PERF_HOT_LOOPS;
  if (!RunHotSpillScanner && !RunHotLoopScanner)
    return Error::success();

  const uint64_t HotThreshold = std::max<uint64_t>(BC.getHotThreshold(), 1);
  std::vector<HotSpillReport> Reports;
  std::vector<HotLoopReport> LoopReports;

  for (BinaryFunction *BF : BC.getAllBinaryFunctions()) {
    if (!BF || BF->isIgnored())
      continue;

    if (RunHotSpillScanner) {
      iterateOverInstrs(*BF, [&](MCInstReference InstRef) {
        if (std::optional<HotSpillReport> Report =
                analyzeHotSpill(*BF, InstRef, HotThreshold))
          Reports.push_back(*Report);
      });
    }

    if (RunHotLoopScanner && BF->hasCFG()) {
      BF->getLayout().updateLayoutIndices();
      for (BinaryBasicBlock &Latch : *BF) {
        for (BinaryBasicBlock *Succ : Latch.successors()) {
          if (Succ->getLayoutIndex() > Latch.getLayoutIndex())
            continue;
          if (std::optional<HotLoopReport> Report =
                  analyzeHotLoop(*BF, Latch, *Succ, HotThreshold))
            LoopReports.push_back(*Report);
        }
      }
    }
  }

  llvm::sort(Reports, [](const HotSpillReport &A, const HotSpillReport &B) {
    if (A.ExecutionCount != B.ExecutionCount)
      return A.ExecutionCount > B.ExecutionCount;
    return A.Location.computeAddress() < B.Location.computeAddress();
  });

  llvm::sort(LoopReports, [](const HotLoopReport &A, const HotLoopReport &B) {
    if (A.LoopHeatCount != B.LoopHeatCount)
      return A.LoopHeatCount > B.LoopHeatCount;
    return A.Header->getInputOffset() < B.Header->getInputOffset();
  });

  SmallVector<FunctionAdviceReport, 8> FunctionReports;
  auto GetOrCreateFunctionReport =
      [&](BinaryFunction *BF) -> FunctionAdviceReport & {
    auto It =
        llvm::find_if(FunctionReports, [&](const FunctionAdviceReport &R) {
          return R.Function == BF;
        });
    if (It != FunctionReports.end())
      return *It;
    FunctionReports.push_back(FunctionAdviceReport{BF});
    FunctionReports.back().Heat =
        std::max(BF->getKnownExecutionCount(), BF->getRawSampleCount());
    return FunctionReports.back();
  };

  for (const HotLoopReport &Report : LoopReports) {
    FunctionAdviceReport &FunctionReport =
        GetOrCreateFunctionReport(Report.Function);
    FunctionReport.Loops.push_back(&Report);
    FunctionReport.Heat = std::max(FunctionReport.Heat, Report.LoopHeatCount);
  }

  for (const HotSpillReport &Report : Reports) {
    FunctionAdviceReport &FunctionReport = GetOrCreateFunctionReport(
        const_cast<BinaryFunction *>(Report.Location.getFunction()));
    FunctionReport.Spills.push_back(&Report);
    FunctionReport.Heat = std::max(FunctionReport.Heat, Report.ExecutionCount);
  }

  llvm::sort(FunctionReports,
             [](const FunctionAdviceReport &A, const FunctionAdviceReport &B) {
               if (A.Heat != B.Heat)
                 return A.Heat > B.Heat;
               return A.Function->getPrintName() < B.Function->getPrintName();
             });

  if (FunctionReports.empty())
    return Error::success();

  std::error_code EC;
  raw_fd_ostream ReportOS(PerfAdvisorReportFilename, EC, sys::fs::OF_Text);
  if (EC)
    return errorCodeToError(EC);

  printFunctionAdviceReport(ReportOS, BC, FunctionReports);
  ReportOS.close();
  if (ReportOS.has_error())
    return errorCodeToError(ReportOS.error());

  BC.outs() << "BOLT-INFO: wrote performance advisor report to "
            << PerfAdvisorReportFilename << "\n";

  return Error::success();
}

} // namespace bolt
} // namespace llvm
