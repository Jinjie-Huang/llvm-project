//===- InstrumentWhatUNeedRuntimeLibrary.cpp ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/RuntimeLibs/InstrumentWhatUNeedRuntimeLibrary.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/BinarySection.h"
#include "bolt/Core/Linker.h"
#include "bolt/Utils/CommandLineOpts.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace bolt;

namespace opts {
cl::opt<std::string> RuntimeInstrumentWhatUNeedLib(
    "runtime-instrument-func-probe-lib",
    cl::desc("specify path of the func-probe resolver library"),
    cl::init("libbolt_rt_iwyn.a"), cl::cat(BoltInstrCategory));
} // namespace opts

bool InstrumentWhatUNeedRuntimeLibrary::needsRuntime(const BinaryContext &BC) {
  if (!opts::isFuncProbeInstrumentation())
    return false;
  const BinaryData *Data =
      BC.getBinaryDataByName(opts::InstrumentFuncProbeFunction);
  return !Data || !BC.getFunctionForSymbol(Data->getSymbol());
}

void InstrumentWhatUNeedRuntimeLibrary::adjustCommandLineOptions(
    const BinaryContext &BC) const {
  if (!BC.isELF() || BC.IsStaticExecutable || !BC.HasInterpHeader) {
    errs() << "BOLT-ERROR: --instrument=func-probe with an external hook "
              "requires a dynamically linked ELF executable\n";
    exit(1);
  }
}

void InstrumentWhatUNeedRuntimeLibrary::emitBinary(BinaryContext &BC,
                                                   MCStreamer &Streamer) {
  MCSection *Section = BC.Ctx->getELFSection(
      ".bolt.iwyn", ELF::SHT_PROGBITS,
      BinarySection::getFlags(/*IsReadOnly=*/false, /*IsText=*/false,
                              /*IsAllocatable=*/true));
  Section->setAlignment(Align(8));
  Streamer.switchSection(Section);

  auto EmitGlobalLabel = [&](StringRef Name) {
    MCSymbol *Symbol = BC.Ctx->getOrCreateSymbol(Name);
    Streamer.emitSymbolAttribute(Symbol, MCSymbolAttr::MCSA_Global);
    Streamer.emitLabel(Symbol);
  };

  EmitGlobalLabel(DispatchOffsetName);
  Streamer.emitIntValue(0, 8);
  EmitGlobalLabel(HookName);
  Streamer.emitBytes(opts::InstrumentFuncProbeFunction);
  Streamer.emitIntValue(0, 1);
}

void InstrumentWhatUNeedRuntimeLibrary::link(
    BinaryContext &BC, StringRef ToolPath, BOLTLinker &Linker,
    BOLTLinker::SectionsMapper MapSections) {
  std::string LibPath =
      getLibPath(ToolPath, opts::RuntimeInstrumentWhatUNeedLib);
  loadLibrary(LibPath, Linker, MapSections);

  const auto Dispatch = Linker.lookupSymbolInfo("__bolt_iwyn_dispatch");
  const auto DispatchOffset = Linker.lookupSymbolInfo(DispatchOffsetName);
  const auto DynamicOffset = Linker.lookupSymbolInfo(DynamicOffsetName);
  ErrorOr<BinarySection &> DynamicSection =
      BC.getUniqueSectionByName(".dynamic");
  if (!Dispatch || !DispatchOffset || !DynamicOffset || !DynamicSection) {
    errs() << "BOLT-ERROR: cannot initialize func-probe runtime\n";
    exit(1);
  }

  auto WriteRelativeOffset = [&](uint64_t SlotAddress, uint64_t TargetAddress) {
    for (BinarySection &Section : BC.allocatableSections()) {
      const uint64_t Start = Section.getOutputAddress();
      if (!Section.isFinalized() || !Section.getOutputData() ||
          !(Start <= SlotAddress &&
            SlotAddress + 8 <= Start + Section.getOutputSize()))
        continue;
      support::endian::write64le(Section.getOutputData() + SlotAddress - Start,
                                 TargetAddress - SlotAddress);
      return true;
    }
    return false;
  };

  if (!WriteRelativeOffset(DispatchOffset->Address, Dispatch->Address) ||
      !WriteRelativeOffset(DynamicOffset->Address,
                           DynamicSection->getAddress())) {
    errs() << "BOLT-ERROR: cannot locate func-probe runtime slot\n";
    exit(1);
  }
}
