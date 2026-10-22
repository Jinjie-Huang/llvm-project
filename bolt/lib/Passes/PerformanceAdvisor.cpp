//===- bolt/Passes/PerformanceAdvisor.cpp ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Passes/PerformanceAdvisor.h"
#include "bolt/Core/BinaryLoop.h"
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
#include "llvm/Support/GenericLoopInfoImpl.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"

#define DEBUG_TYPE "bolt-performance-advisor"

namespace llvm {
namespace bolt {

namespace {

static cl::opt<std::string> PerfAdvisorReportFilename(
    "perf-advisor-report",
    cl::desc("write profile-correlated performance advisor JSON report to "
             "file"),
    cl::init("perf-advisor-report.json"),
    cl::cat(opts::BinaryAnalysisCategory));

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
  SmallVector<BinaryBasicBlock *, 2> Latches;
  uint64_t HeaderExecutionCount{0};
  uint64_t BackedgeCount{0};
  uint64_t EntryCount{0};
  uint64_t LoopHeatCount{0};
  std::optional<double> EstimatedTripCount;
  bool HasBackedgeProfile{false};
  bool ProfileConsistent{false};
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

static LoopStats collectLoopStats(const BinaryContext &BC,
                                  const BinaryLoop &Loop) {
  LoopStats Stats;
  Stats.BodyBlockCount = Loop.getNumBlocks();
  SmallSet<MCPhysReg, 32> TouchedRegisters;

  for (const BinaryBasicBlock *BB : Loop.blocks()) {
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
      if (Loop.contains(Succ))
        continue;
      ++Stats.ExitEdgeCount;
      if (BB != Loop.getHeader() && !Loop.isLoopLatch(BB))
        Stats.HasEarlyExit = true;
    }
  }

  Stats.RegisterCount = TouchedRegisters.size();
  if (Stats.ExitEdgeCount > 1)
    Stats.HasEarlyExit = true;
  return Stats;
}

static std::optional<HotLoopReport> analyzeHotLoop(BinaryFunction &BF,
                                                   const BinaryLoop &Loop,
                                                   uint64_t HotThreshold) {
  HotLoopReport Report;
  Report.Function = &BF;
  Report.Header = Loop.getHeader();
  Loop.getLoopLatches(Report.Latches);
  llvm::sort(Report.Latches,
             [](const BinaryBasicBlock *Left, const BinaryBasicBlock *Right) {
               return Left->getInputOffset() < Right->getInputOffset();
             });
  Report.HeaderExecutionCount = Report.Header->getKnownExecutionCount();
  Report.HasBackedgeProfile = BF.getProfileFlags() & BinaryFunction::PF_BRANCH;
  bool CountsValid = Report.HasBackedgeProfile;
  auto AddCount = [&](uint64_t &Total, uint64_t Count) {
    if (Count == BinaryBasicBlock::COUNT_NO_PROFILE) {
      CountsValid = false;
      return;
    }
    bool Overflow = false;
    Total = SaturatingAdd(Total, Count, &Overflow);
    CountsValid &= !Overflow;
  };
  for (const BinaryBasicBlock *Pred : Report.Header->predecessors()) {
    const uint64_t Count = Pred->getBranchInfo(*Report.Header).Count;
    if (Loop.contains(Pred)) {
      if (Count == BinaryBasicBlock::COUNT_NO_PROFILE)
        Report.HasBackedgeProfile = false;
      AddCount(Report.BackedgeCount, Count);
    } else {
      AddCount(Report.EntryCount, Count);
    }
  }
  for (const BinaryBasicBlock *BB : Loop.blocks()) {
    if (BB->isLandingPad() || (BB->isEntryPoint() && BB != &BF.front()))
      CountsValid = false;
  }
  if (Report.Header == &BF.front()) {
    CountsValid &= !BF.isMultiEntry();
    AddCount(Report.EntryCount, BF.getKnownExecutionCount());
  }
  uint64_t ExpectedHeaderCount = Report.EntryCount;
  AddCount(ExpectedHeaderCount, Report.BackedgeCount);
  if (Report.Header == &BF.front() &&
      Report.HeaderExecutionCount <= ExpectedHeaderCount)
    Report.HeaderExecutionCount = ExpectedHeaderCount;
  Report.ProfileConsistent = CountsValid && Report.Header->hasProfile() &&
                             Report.HeaderExecutionCount == ExpectedHeaderCount;
  if (Report.ProfileConsistent && Report.EntryCount)
    Report.EstimatedTripCount =
        1.0 + static_cast<double>(Report.BackedgeCount) / Report.EntryCount;
  Report.LoopHeatCount =
      std::max(Report.BackedgeCount, Report.HeaderExecutionCount);
  if (Report.LoopHeatCount < HotThreshold)
    return std::nullopt;

  const BinaryContext &BC = BF.getBinaryContext();
  LoopStats Stats = collectLoopStats(BC, Loop);
  Report.Stats = Stats;

  const bool SmallEnoughToUnroll = Stats.InstructionCount <= 32;
  const bool RegisterPressureOK = Stats.RegisterCount <= 24;
  Report.SuggestUnroll = SmallEnoughToUnroll && !Stats.HasEarlyExit &&
                         Stats.CallCount == 0 && RegisterPressureOK;

  const bool HasMemoryTraffic = Stats.LoadCount + Stats.StoreCount >= 2;
  const bool HasCompute =
      llvm::any_of(Loop.blocks(), [&](BinaryBasicBlock *BB) {
        return llvm::any_of(*BB, [&](const MCInst &Inst) {
          return isArithmeticLikeInstruction(BC, Inst);
        });
      });
  const bool ScalarHotLoop = Stats.VectorInstructionCount == 0;
  Report.SuggestVectorize =
      ScalarHotLoop && HasMemoryTraffic && HasCompute && Stats.CallCount == 0;

  Report.LikelyAliasOrDependenceBlocker =
      Report.SuggestVectorize && Stats.LoadCount > 0 && Stats.StoreCount > 0;
  Report.LikelyTripCountBlocker =
      Report.SuggestVectorize &&
      (!Report.EstimatedTripCount || *Report.EstimatedTripCount < 8);
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
    // Use DWARFContext's address-to-CU index for normal binaries. Scanning
    // every compile unit for every finding is prohibitively expensive in
    // large binaries. The raw line-table fallback below is only needed when
    // there are no compile units to anchor the lookup.
    if (std::optional<DILineInfo> LineInfo =
            BC.DwCtx->getLineInfoForAddress(object::SectionedAddress{
                Address, object::SectionedAddress::UndefSection});
        LineInfo && LineInfo->FileName != DILineInfo::BadString)
      if (std::optional<std::string> Loc =
              FormatLine(LineInfo->FileName, LineInfo->Line, LineInfo->Column))
        return Loc;

    // Hand-written assembly tests, and some stripped binaries, can contain a
    // usable .debug_line section without any .debug_info compile units. In that
    // case getLineTableForUnit()/getLineInfoForAddress() have nothing to anchor
    // on, so parse the raw line section directly and search every table.
    if (BC.DwCtx->getNumCompileUnits() != 0)
      return std::nullopt;

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

    return std::nullopt;
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

static std::string formatAddress(uint64_t Address) {
  std::string Result;
  raw_string_ostream OS(Result);
  OS << "0x" << llvm::format("%x", Address);
  return Result;
}

static std::string formatInstruction(const BinaryContext &BC,
                                     const MCInstReference &Location,
                                     uint64_t Address,
                                     const BinaryFunction *BF) {
  std::string Result;
  raw_string_ostream OS(Result);
  BC.printInstruction(OS, Location, Address, BF);
  return StringRef(OS.str()).trim().str();
}

static void writeHotSpillJSON(json::OStream &J, const BinaryContext &BC,
                              const HotSpillReport &Report) {
  const BinaryFunction *BF = Report.Location.getFunction();
  const BinaryBasicBlock *BB = Report.Location.getBasicBlock();
  const uint64_t Address = Report.Location.computeAddress();
  const uint64_t ReloadAddress = Report.ReloadLocation.computeAddress();

  J.object([&] {
    if (BB)
      J.attribute("basic_block", BB->getName());
    J.attribute("address", formatAddress(Address));
    if (std::optional<std::string> SourceLoc =
            getHotSpillSourceLocation(BC, Report))
      J.attribute("source", *SourceLoc);
    J.attribute("instruction",
                formatInstruction(BC, Report.Location, Address, BF));
    J.attribute("execution_count", Report.ExecutionCount);
    J.attributeObject("store", [&] {
      J.attribute("register", BC.MRI->getName(Report.SrcReg));
      J.attribute("stack_base", Report.StackPtrReg
                                    ? BC.MRI->getName(Report.StackPtrReg)
                                    : "unknown-base");
      J.attribute("stack_offset", Report.StackOffset);
      J.attribute("size", Report.Size);
      J.attribute("stack_slot",
                  formatStackSlot(BC, Report.StackPtrReg, Report.StackOffset));
    });
    J.attributeObject("reload", [&] {
      J.attribute("register", BC.MRI->getName(Report.ReloadReg));
      J.attribute("address", formatAddress(ReloadAddress));
      J.attribute("instruction", formatInstruction(BC, Report.ReloadLocation,
                                                   ReloadAddress, BF));
      if (std::optional<std::string> SourceLoc = getSourceLocation(
              BC, Report.ReloadLocation.getMCInst(), ReloadAddress))
        J.attribute("source", *SourceLoc);
    });
  });
}

static void writeHotLoopJSON(json::OStream &J, const BinaryContext &BC,
                             const HotLoopReport &Report) {
  J.object([&] {
    J.attribute("header", Report.Header->getName());
    J.attribute("header_address",
                formatAddress(Report.Function->getAddress() +
                              Report.Header->getInputOffset()));
    if (Report.Latches.size() == 1)
      J.attribute("latch", Report.Latches.front()->getName());
    else
      J.attribute("latch", nullptr);
    J.attributeArray("latches", [&] {
      for (const BinaryBasicBlock *Latch : Report.Latches)
        J.object([&] {
          J.attribute("name", Latch->getName());
          J.attribute("address", formatAddress(Report.Function->getAddress() +
                                               Latch->getInputOffset()));
        });
    });
    if (std::optional<std::string> SourceLoc =
            getHotLoopSourceLocation(BC, Report))
      J.attribute("source", *SourceLoc);
    J.attributeObject("heat", [&] {
      J.attribute("header_execution_count", Report.HeaderExecutionCount);
      J.attribute("backedge_count", Report.BackedgeCount);
      J.attribute("entry_count", Report.EntryCount);
      J.attribute("loop_heat_count", Report.LoopHeatCount);
      J.attribute("has_backedge_profile", Report.HasBackedgeProfile);
      J.attribute("profile_consistent", Report.ProfileConsistent);
      if (Report.EstimatedTripCount)
        J.attribute("estimated_trip_count", *Report.EstimatedTripCount);
      else
        J.attribute("estimated_trip_count", nullptr);
    });
    J.attributeObject("shape", [&] {
      J.attribute("body_block_count", Report.Stats.BodyBlockCount);
      J.attribute("instruction_count", Report.Stats.InstructionCount);
      J.attribute("exit_edge_count", Report.Stats.ExitEdgeCount);
      J.attribute("conditional_branch_count",
                  Report.Stats.ConditionalBranchCount);
      J.attribute("call_count", Report.Stats.CallCount);
      J.attribute("load_count", Report.Stats.LoadCount);
      J.attribute("store_count", Report.Stats.StoreCount);
      J.attribute("vector_instruction_count",
                  Report.Stats.VectorInstructionCount);
      J.attribute("register_count", Report.Stats.RegisterCount);
      J.attribute("has_early_exit", Report.Stats.HasEarlyExit);
    });
    J.attributeArray("recommendations", [&] {
      if (Report.SuggestUnroll)
        J.value("unroll");
      if (Report.SuggestVectorize)
        J.value("vectorize");
    });
    J.attributeArray("likely_blockers", [&] {
      if (Report.LikelyAliasOrDependenceBlocker)
        J.value("alias_or_dependence");
      if (Report.LikelyTripCountBlocker)
        J.value("trip_count");
      if (Report.LikelyControlFlowBlocker)
        J.value("control_flow");
    });
  });
}

static void writePerformanceAdvisorJSON(raw_ostream &OS,
                                        const BinaryContext &BC,
                                        ArrayRef<FunctionAdviceReport> Reports,
                                        uint64_t HotThreshold,
                                        size_t HotSpillCount,
                                        size_t HotLoopCount) {
  json::OStream J(OS, 2);
  J.object([&] {
    J.attribute("report", "llvm-bolt-performance-advisor");
    J.attribute("schema_version", 2);
    J.attribute("hot_threshold", HotThreshold);
    J.attribute("sort_order", "function_heat_descending");
    J.attributeObject("summary", [&] {
      J.attribute("function_count", Reports.size());
      J.attribute("hint_count", HotSpillCount + HotLoopCount);
      J.attribute("hot_stack_slot_count", HotSpillCount);
      J.attribute("hot_loop_count", HotLoopCount);
    });
    J.attributeObject("advice", [&] {
      J.attribute(
          "hot_stack_slot",
          "This is spill-like hot stack traffic, not proof of a compiler "
          "spill. Check whether the source value is address-taken, escaped, "
          "volatile/atomic, ABI/debug home storage, or alloca-backed memory. "
          "Otherwise reduce register pressure, shorten live ranges, split "
          "complex expressions, enable stronger optimization, or restructure "
          "the code to keep the value in a register.");
      J.attribute(
          "unroll",
          "Consider source-level unrolling, `#pragma clang loop "
          "unroll(enable)`, or profile-guided unroll threshold tuning.");
      J.attribute(
          "vectorize",
          "Check why the compiler kept the loop scalar. Use optimization "
          "remarks (`-Rpass-missed=loop-vectorize`) to confirm the exact "
          "compiler-side reason.");
      J.attribute(
          "alias_or_dependence",
          "If source semantics allow it, use `__restrict__`, alignment "
          "assumptions, or split arrays to make independence explicit.");
      J.attribute(
          "trip_count",
          "Pass PGO, enable loop vectorization, or expose the minimum trip "
          "count in the source.");
      J.attribute(
          "control_flow",
          "Consider if-conversion, peeling rare exits, or separating the hot "
          "straight-line path.");
    });
    J.attributeArray("functions", [&] {
      for (const FunctionAdviceReport &FunctionReport : Reports) {
        const BinaryFunction *BF = FunctionReport.Function;
        J.object([&] {
          J.attribute("name", BF->getPrintName());
          J.attribute("heat", FunctionReport.Heat);
          J.attribute("entry_count", BF->getKnownExecutionCount());
          J.attribute("raw_sample_count", BF->getRawSampleCount());
          J.attributeArray("hot_loops", [&] {
            for (const HotLoopReport *Loop : FunctionReport.Loops)
              writeHotLoopJSON(J, BC, *Loop);
          });
          J.attributeArray("hot_stack_slots", [&] {
            for (const HotSpillReport *Spill : FunctionReport.Spills)
              writeHotSpillJSON(J, BC, *Spill);
          });
        });
      }
    });
  });
  OS << '\n';
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

  uint64_t TotalExecutionCount = 0;
  uint64_t ProfiledFunctionCount = 0;
  for (const BinaryFunction *BF : BC.getAllBinaryFunctions()) {
    if (!BF || BF->isIgnored() || !BF->hasValidProfile())
      continue;
    TotalExecutionCount =
        SaturatingAdd(TotalExecutionCount, BF->getKnownExecutionCount());
    ++ProfiledFunctionCount;
  }
  const uint64_t HotThreshold = std::max(
      {uint64_t{1}, uint64_t{opts::ExecutionCountThreshold},
       ProfiledFunctionCount ? TotalExecutionCount / ProfiledFunctionCount / 2
                             : 0});
  std::vector<HotSpillReport> Reports;
  std::vector<HotLoopReport> LoopReports;

  for (BinaryFunction *BF : BC.getAllBinaryFunctions()) {
    if (!BF || BF->isIgnored() || !BF->hasValidProfile())
      continue;

    if (RunHotSpillScanner) {
      iterateOverInstrs(*BF, [&](MCInstReference InstRef) {
        if (std::optional<HotSpillReport> Report =
                analyzeHotSpill(*BF, InstRef, HotThreshold))
          Reports.push_back(*Report);
      });
    }

    if (RunHotLoopScanner && BF->hasCFG()) {
      if (!BF->hasDomTree())
        BF->constructDomTree();
      BinaryLoopInfo LoopInfo;
      LoopInfo.analyze(BF->getDomTree());
      for (const BinaryLoop *Loop : LoopInfo.getLoopsInPreorder()) {
        if (std::optional<HotLoopReport> Report =
                analyzeHotLoop(*BF, *Loop, HotThreshold))
          LoopReports.push_back(*Report);
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
    if (A.Function->getAddress() != B.Function->getAddress())
      return A.Function->getAddress() < B.Function->getAddress();
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

  std::error_code EC;
  raw_fd_ostream ReportOS(PerfAdvisorReportFilename, EC, sys::fs::OF_Text);
  if (EC)
    return errorCodeToError(EC);

  writePerformanceAdvisorJSON(ReportOS, BC, FunctionReports, HotThreshold,
                              Reports.size(), LoopReports.size());
  ReportOS.close();
  if (ReportOS.has_error())
    return errorCodeToError(ReportOS.error());

  BC.outs() << "BOLT-INFO: wrote performance advisor report to "
            << PerfAdvisorReportFilename << "\n";

  return Error::success();
}

} // namespace bolt
} // namespace llvm
