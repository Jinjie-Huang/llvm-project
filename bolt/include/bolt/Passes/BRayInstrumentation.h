//===- bolt/Passes/BRayInstrumentation.h ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef BOLT_PASSES_BRAY_INSTRUMENTATION_H
#define BOLT_PASSES_BRAY_INSTRUMENTATION_H

#include "bolt/Passes/BinaryPasses.h"

namespace llvm {
namespace bolt {

class BRayInstrumentation : public BinaryFunctionPass {
public:
  explicit BRayInstrumentation(const cl::opt<bool> &PrintPass)
      : BinaryFunctionPass(PrintPass) {}

  const char *getName() const override { return "bray-instrumentation"; }
  Error runOnFunctions(BinaryContext &BC) override;
};

} // namespace bolt
} // namespace llvm

#endif
