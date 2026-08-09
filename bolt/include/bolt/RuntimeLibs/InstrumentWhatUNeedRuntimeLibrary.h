//===- InstrumentWhatUNeedRuntimeLibrary.h ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_RUNTIMELIBS_INSTRUMENT_WHAT_U_NEED_RUNTIME_LIBRARY_H
#define BOLT_RUNTIMELIBS_INSTRUMENT_WHAT_U_NEED_RUNTIME_LIBRARY_H

#include "bolt/RuntimeLibs/RuntimeLibrary.h"

namespace llvm {
namespace bolt {

class InstrumentWhatUNeedRuntimeLibrary : public RuntimeLibrary {
public:
  static constexpr StringLiteral DispatchOffsetName =
      "__bolt_iwyn_dispatch_offset";
  static constexpr StringLiteral DynamicOffsetName =
      "__bolt_iwyn_dynamic_offset";
  static constexpr StringLiteral HookName = "__bolt_iwyn_hook_name";

  void addRuntimeLibSections(std::vector<std::string> &SecNames) const final {
    SecNames.push_back(".bolt.iwyn");
  }

  void adjustCommandLineOptions(const BinaryContext &BC) const final;
  void emitBinary(BinaryContext &BC, MCStreamer &Streamer) final;
  void link(BinaryContext &BC, StringRef ToolPath, BOLTLinker &Linker,
            BOLTLinker::SectionsMapper MapSections) final;

  static bool needsRuntime(const BinaryContext &BC);
};

} // namespace bolt
} // namespace llvm

#endif
