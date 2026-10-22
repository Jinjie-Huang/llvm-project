//===- bolt/Passes/PerformanceAdvisor.h -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_PERFORMANCEADVISOR_H
#define BOLT_PASSES_PERFORMANCEADVISOR_H

#include "bolt/Passes/BinaryPasses.h"
#include "bolt/Utils/CommandLineOpts.h"

namespace llvm {
namespace bolt {

class PerformanceAdvisor : public BinaryFunctionPass {
  opts::GadgetKindBitmask EnabledScanners;

public:
  explicit PerformanceAdvisor(opts::GadgetKindBitmask EnabledScanners);

  const char *getName() const override { return "performance-advisor"; }

  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif
