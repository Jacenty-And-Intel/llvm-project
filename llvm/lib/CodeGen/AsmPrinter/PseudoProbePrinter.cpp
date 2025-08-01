//===- llvm/CodeGen/PseudoProbePrinter.cpp - Pseudo Probe Emission -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains support for writing pseudo probe info into asm files.
//
//===----------------------------------------------------------------------===//

#include "PseudoProbePrinter.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/PseudoProbe.h"
#include "llvm/MC/MCPseudoProbe.h"
#include "llvm/MC/MCStreamer.h"

#ifndef NDEBUG
#include "llvm/IR/Module.h"
#include "llvm/Support/WithColor.h"
#endif

using namespace llvm;

void PseudoProbeHandler::emitPseudoProbe(uint64_t Guid, uint64_t Index,
                                         uint64_t Type, uint64_t Attr,
                                         const DILocation *DebugLoc) {
  // Gather all the inlined-at nodes.
  // When it's done ReversedInlineStack looks like ([66, B], [88, A])
  // which means, Function A inlines function B at calliste with a probe id 88,
  // and B inlines C at probe 66 where C is represented by Guid.
  SmallVector<InlineSite, 8> ReversedInlineStack;
  auto *InlinedAt = DebugLoc ? DebugLoc->getInlinedAt() : nullptr;
  while (InlinedAt) {
    auto Name = InlinedAt->getSubprogramLinkageName();
    // Use caching to avoid redundant md5 computation for build speed.
    uint64_t &CallerGuid = NameGuidMap[Name];
    if (!CallerGuid)
      CallerGuid = Function::getGUIDAssumingExternalLinkage(Name);
#ifndef NDEBUG
    verifyGuidExistenceInDesc(CallerGuid, Name);
#endif
    uint64_t CallerProbeId = PseudoProbeDwarfDiscriminator::extractProbeIndex(
        InlinedAt->getDiscriminator());
    ReversedInlineStack.emplace_back(CallerGuid, CallerProbeId);
    InlinedAt = InlinedAt->getInlinedAt();
  }
  uint64_t Discriminator = 0;
  // For now only block probes have FS discriminators. See
  // MIRFSDiscriminator.cpp for more details.
  if (EnableFSDiscriminator && DebugLoc &&
      (Type == (uint64_t)PseudoProbeType::Block))
    Discriminator = DebugLoc->getDiscriminator();
  assert((EnableFSDiscriminator || Discriminator == 0) &&
         "Discriminator should not be set in non-FSAFDO mode");
  SmallVector<InlineSite, 8> InlineStack(llvm::reverse(ReversedInlineStack));
  Asm->OutStreamer->emitPseudoProbe(Guid, Index, Type, Attr, Discriminator,
                                    InlineStack, Asm->CurrentFnSym);
#ifndef NDEBUG
  verifyGuidExistenceInDesc(
      Guid, DebugLoc ? DebugLoc->getSubprogramLinkageName() : "");
#endif
}

#ifndef NDEBUG
void PseudoProbeHandler::verifyGuidExistenceInDesc(uint64_t Guid,
                                                   StringRef FuncName) {
  NamedMDNode *Desc = Asm->MF->getFunction().getParent()->getNamedMetadata(
      PseudoProbeDescMetadataName);
  assert(Desc && "pseudo probe does not exist");

  // Keep DescGuidSet up to date.
  for (size_t I = DescGuidSet.size(), E = Desc->getNumOperands(); I != E; ++I) {
    const auto *MD = cast<MDNode>(Desc->getOperand(I));
    auto *ID = mdconst::extract<ConstantInt>(MD->getOperand(0));
    DescGuidSet.insert(ID->getZExtValue());
  }

  if (!DescGuidSet.contains(Guid))
    WithColor::warning() << "Guid:" << Guid << " Name:" << FuncName
                         << " does not exist in pseudo probe desc\n";
}
#endif
