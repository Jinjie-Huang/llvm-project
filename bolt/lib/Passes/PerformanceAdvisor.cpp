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
#include "llvm/DebugInfo/DIContext.h"
#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "llvm/DebugInfo/DWARF/DWARFObject.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Format.h"

#define DEBUG_TYPE "bolt-performance-advisor"

namespace llvm {
namespace bolt {

namespace {

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

static void printHotSpillReport(raw_ostream &OS, const BinaryContext &BC,
                                const HotSpillReport &Report) {
  const BinaryFunction *BF = Report.Location.getFunction();
  const BinaryBasicBlock *BB = Report.Location.getBasicBlock();
  const uint64_t Address = Report.Location.computeAddress();
  const uint64_t ReloadAddress = Report.ReloadLocation.computeAddress();

  OS << "\nPERF-ADVISOR: hot stack slot traffic in function "
     << BF->getPrintName();
  if (BB)
    OS << ", basic block " << BB->getName();
  OS << ", at address " << llvm::format("%x", Address) << "\n";

  OS << "  The instruction is ";
  BC.printInstruction(OS, Report.Location, Address, BF);
  if (std::optional<std::string> SourceLoc =
          getSourceLocation(BC, Report.Location.getMCInst(), Address))
    OS << "  Source: " << *SourceLoc << "\n";

  OS << "  Profile: containing block/function execution count is "
     << Report.ExecutionCount << "; function entry count is "
     << BF->getKnownExecutionCount() << ".\n";
  OS << "  Stack traffic: stores " << BC.MRI->getName(Report.SrcReg) << " to "
     << formatStackSlot(BC, Report.StackPtrReg, Report.StackOffset) << " ("
     << static_cast<unsigned>(Report.Size) << " bytes).\n";
  OS << "  Evidence: the same stack slot is later reloaded into "
     << BC.MRI->getName(Report.ReloadReg) << " at address "
     << llvm::format("%x", ReloadAddress) << ": ";
  BC.printInstruction(OS, Report.ReloadLocation, ReloadAddress, BF);
  if (std::optional<std::string> SourceLoc = getSourceLocation(
          BC, Report.ReloadLocation.getMCInst(), ReloadAddress))
    OS << "  Reload source: " << *SourceLoc << "\n";
  OS << "  Hint: this is a spill-like hot stack slot, not a proof that the "
        "compiler spilled a value. RBP/RSP-relative slots can also be real "
        "locals, and such locals are still optimization candidates when they "
        "do not need a stable memory address. Check whether the source-level "
        "value is address-taken/escaped, volatile/atomic, ABI/debug home "
        "storage, or alloca-backed memory. If not, reducing register pressure, "
        "shortening live ranges, splitting complex expressions, enabling "
        "stronger optimization, or restructuring the code may keep the value "
        "in a register and remove this hot stack traffic.\n";
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

  if (!(EnabledScanners & opts::GS_PERF_HOT_SPILLS))
    return Error::success();

  const uint64_t HotThreshold = std::max<uint64_t>(BC.getHotThreshold(), 1);
  std::vector<HotSpillReport> Reports;

  for (BinaryFunction *BF : BC.getAllBinaryFunctions()) {
    if (!BF || BF->isIgnored())
      continue;

    iterateOverInstrs(*BF, [&](MCInstReference InstRef) {
      if (std::optional<HotSpillReport> Report =
              analyzeHotSpill(*BF, InstRef, HotThreshold))
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
