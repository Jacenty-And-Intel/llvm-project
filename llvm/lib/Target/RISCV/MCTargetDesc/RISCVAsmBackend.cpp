//===-- RISCVAsmBackend.cpp - RISC-V Assembler Backend --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RISCVAsmBackend.h"
#include "RISCVFixupKinds.h"
#include "llvm/ADT/APInt.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

// Temporary workaround for old linkers that do not support ULEB128 relocations,
// which are abused by DWARF v5 DW_LLE_offset_pair/DW_RLE_offset_pair
// implemented in Clang/LLVM.
static cl::opt<bool> ULEB128Reloc(
    "riscv-uleb128-reloc", cl::init(true), cl::Hidden,
    cl::desc("Emit R_RISCV_SET_ULEB128/E_RISCV_SUB_ULEB128 if appropriate"));

RISCVAsmBackend::RISCVAsmBackend(const MCSubtargetInfo &STI, uint8_t OSABI,
                                 bool Is64Bit, const MCTargetOptions &Options)
    : MCAsmBackend(llvm::endianness::little), STI(STI), OSABI(OSABI),
      Is64Bit(Is64Bit), TargetOptions(Options) {
  RISCVFeatures::validate(STI.getTargetTriple(), STI.getFeatureBits());
}

std::optional<MCFixupKind> RISCVAsmBackend::getFixupKind(StringRef Name) const {
  if (STI.getTargetTriple().isOSBinFormatELF()) {
    unsigned Type;
    Type = llvm::StringSwitch<unsigned>(Name)
#define ELF_RELOC(NAME, ID) .Case(#NAME, ID)
#include "llvm/BinaryFormat/ELFRelocs/RISCV.def"
#undef ELF_RELOC
#define ELF_RISCV_NONSTANDARD_RELOC(_VENDOR, NAME, ID) .Case(#NAME, ID)
#include "llvm/BinaryFormat/ELFRelocs/RISCV_nonstandard.def"
#undef ELF_RISCV_NONSTANDARD_RELOC
               .Case("BFD_RELOC_NONE", ELF::R_RISCV_NONE)
               .Case("BFD_RELOC_32", ELF::R_RISCV_32)
               .Case("BFD_RELOC_64", ELF::R_RISCV_64)
               .Default(-1u);
    if (Type != -1u)
      return static_cast<MCFixupKind>(FirstLiteralRelocationKind + Type);
  }
  return std::nullopt;
}

MCFixupKindInfo RISCVAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  const static MCFixupKindInfo Infos[] = {
      // This table *must* be in the order that the fixup_* kinds are defined in
      // RISCVFixupKinds.h.
      //
      // name                      offset bits  flags
      {"fixup_riscv_hi20", 12, 20, 0},
      {"fixup_riscv_lo12_i", 20, 12, 0},
      {"fixup_riscv_12_i", 20, 12, 0},
      {"fixup_riscv_lo12_s", 0, 32, 0},
      {"fixup_riscv_pcrel_hi20", 12, 20, 0},
      {"fixup_riscv_pcrel_lo12_i", 20, 12, 0},
      {"fixup_riscv_pcrel_lo12_s", 0, 32, 0},
      {"fixup_riscv_jal", 12, 20, 0},
      {"fixup_riscv_branch", 0, 32, 0},
      {"fixup_riscv_rvc_jump", 2, 11, 0},
      {"fixup_riscv_rvc_branch", 0, 16, 0},
      {"fixup_riscv_rvc_imm", 0, 16, 0},
      {"fixup_riscv_call", 0, 64, 0},
      {"fixup_riscv_call_plt", 0, 64, 0},

      {"fixup_riscv_qc_e_branch", 0, 48, 0},
      {"fixup_riscv_qc_e_32", 16, 32, 0},
      {"fixup_riscv_qc_abs20_u", 0, 32, 0},
      {"fixup_riscv_qc_e_call_plt", 0, 48, 0},

      // Andes fixups
      {"fixup_riscv_nds_branch_10", 0, 32, 0},
  };
  static_assert((std::size(Infos)) == RISCV::NumTargetFixupKinds,
                "Not all fixup kinds added to Infos array");

  // Fixup kinds from raw relocation types and .reloc directives force
  // relocations and do not use these fields.
  if (mc::isRelocation(Kind))
    return {};

  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);

  assert(unsigned(Kind - FirstTargetFixupKind) < RISCV::NumTargetFixupKinds &&
         "Invalid kind!");
  return Infos[Kind - FirstTargetFixupKind];
}

bool RISCVAsmBackend::fixupNeedsRelaxationAdvanced(const MCFragment &,
                                                   const MCFixup &Fixup,
                                                   const MCValue &,
                                                   uint64_t Value,
                                                   bool Resolved) const {
  int64_t Offset = int64_t(Value);
  auto Kind = Fixup.getKind();

  // Return true if the symbol is unresolved.
  if (!Resolved)
    return true;

  switch (Kind) {
  default:
    return false;
  case RISCV::fixup_riscv_rvc_branch:
    // For compressed branch instructions the immediate must be
    // in the range [-256, 254].
    return Offset > 254 || Offset < -256;
  case RISCV::fixup_riscv_rvc_jump:
    // For compressed jump instructions the immediate must be
    // in the range [-2048, 2046].
    return Offset > 2046 || Offset < -2048;
  case RISCV::fixup_riscv_branch:
  case RISCV::fixup_riscv_qc_e_branch:
    // For conditional branch instructions the immediate must be
    // in the range [-4096, 4094].
    return Offset > 4094 || Offset < -4096;
  case RISCV::fixup_riscv_jal:
    // For jump instructions the immediate must be in the range
    // [-1048576, 1048574]
    return Offset > 1048574 || Offset < -1048576;
  case RISCV::fixup_riscv_rvc_imm:
    // This fixup can never be emitted as a relocation, so always needs to be
    // relaxed.
    return true;
  }
}

// Given a compressed control flow instruction this function returns
// the expanded instruction, or the original instruction code if no
// expansion is available.
static unsigned getRelaxedOpcode(unsigned Opcode, ArrayRef<MCOperand> Operands,
                                 const MCSubtargetInfo &STI) {
  switch (Opcode) {
  case RISCV::C_BEQZ:
    return RISCV::BEQ;
  case RISCV::C_BNEZ:
    return RISCV::BNE;
  case RISCV::C_J:
  case RISCV::C_JAL: // fall through.
    // This only relaxes one "step" - i.e. from C.J to JAL, not from C.J to
    // QC.E.J, because we can always relax again if needed.
    return RISCV::JAL;
  case RISCV::C_LI:
    if (!STI.hasFeature(RISCV::FeatureVendorXqcili))
      break;
    // We only need this because `QC.E.LI` can be compressed into a `C.LI`. This
    // happens because the `simm6` MCOperandPredicate accepts bare symbols, and
    // `QC.E.LI` is the only instruction that accepts bare symbols at parse-time
    // and compresses to `C.LI`. `C.LI` does not itself accept bare symbols at
    // parse time.
    //
    // If we have a bare symbol, we need to turn this back to a `QC.E.LI`, as we
    // have no way to emit a relocation on a `C.LI` instruction.
    return RISCV::QC_E_LI;
  case RISCV::JAL: {
    // We can only relax JAL if we have Xqcilb
    if (!STI.hasFeature(RISCV::FeatureVendorXqcilb))
      break;

    // And only if it is using X0 or X1 for rd.
    MCRegister Reg = Operands[0].getReg();
    if (Reg == RISCV::X0)
      return RISCV::QC_E_J;
    if (Reg == RISCV::X1)
      return RISCV::QC_E_JAL;

    break;
  }
  case RISCV::BEQ:
    return RISCV::PseudoLongBEQ;
  case RISCV::BNE:
    return RISCV::PseudoLongBNE;
  case RISCV::BLT:
    return RISCV::PseudoLongBLT;
  case RISCV::BGE:
    return RISCV::PseudoLongBGE;
  case RISCV::BLTU:
    return RISCV::PseudoLongBLTU;
  case RISCV::BGEU:
    return RISCV::PseudoLongBGEU;
  case RISCV::QC_BEQI:
    return RISCV::PseudoLongQC_BEQI;
  case RISCV::QC_BNEI:
    return RISCV::PseudoLongQC_BNEI;
  case RISCV::QC_BLTI:
    return RISCV::PseudoLongQC_BLTI;
  case RISCV::QC_BGEI:
    return RISCV::PseudoLongQC_BGEI;
  case RISCV::QC_BLTUI:
    return RISCV::PseudoLongQC_BLTUI;
  case RISCV::QC_BGEUI:
    return RISCV::PseudoLongQC_BGEUI;
  case RISCV::QC_E_BEQI:
    return RISCV::PseudoLongQC_E_BEQI;
  case RISCV::QC_E_BNEI:
    return RISCV::PseudoLongQC_E_BNEI;
  case RISCV::QC_E_BLTI:
    return RISCV::PseudoLongQC_E_BLTI;
  case RISCV::QC_E_BGEI:
    return RISCV::PseudoLongQC_E_BGEI;
  case RISCV::QC_E_BLTUI:
    return RISCV::PseudoLongQC_E_BLTUI;
  case RISCV::QC_E_BGEUI:
    return RISCV::PseudoLongQC_E_BGEUI;
  }

  // Returning the original opcode means we cannot relax the instruction.
  return Opcode;
}

void RISCVAsmBackend::relaxInstruction(MCInst &Inst,
                                       const MCSubtargetInfo &STI) const {
  if (STI.hasFeature(RISCV::FeatureExactAssembly))
    return;

  MCInst Res;
  switch (Inst.getOpcode()) {
  default:
    llvm_unreachable("Opcode not expected!");
  case RISCV::C_BEQZ:
  case RISCV::C_BNEZ:
  case RISCV::C_J:
  case RISCV::C_JAL: {
    [[maybe_unused]] bool Success = RISCVRVC::uncompress(Res, Inst, STI);
    assert(Success && "Can't uncompress instruction");
    assert(Res.getOpcode() ==
               getRelaxedOpcode(Inst.getOpcode(), Inst.getOperands(), STI) &&
           "Branch Relaxation Error");
    break;
  }
  case RISCV::JAL: {
    // This has to be written manually because the QC.E.J -> JAL is
    // compression-only, so that it is not used when printing disassembly.
    assert(STI.hasFeature(RISCV::FeatureVendorXqcilb) &&
           "JAL is only relaxable with Xqcilb");
    assert((Inst.getOperand(0).getReg() == RISCV::X0 ||
            Inst.getOperand(0).getReg() == RISCV::X1) &&
           "JAL only relaxable with rd=x0 or rd=x1");
    Res.setOpcode(getRelaxedOpcode(Inst.getOpcode(), Inst.getOperands(), STI));
    Res.addOperand(Inst.getOperand(1));
    break;
  }
  case RISCV::C_LI: {
    // This should only be hit when trying to relax a `C.LI` into a `QC.E.LI`
    // because the `C.LI` has a bare symbol. We cannot use
    // `RISCVRVC::uncompress` because it will use decompression patterns. The
    // `QC.E.LI` compression pattern to `C.LI` is compression-only (because we
    // don't want `c.li` ever printed as `qc.e.li`, which might be done if the
    // pattern applied to decompression), but that doesn't help much becuase
    // `C.LI` with a bare symbol will decompress to an `ADDI` anyway (because
    // `simm12`'s MCOperandPredicate accepts a bare symbol and that pattern
    // comes first), and we still cannot emit an `ADDI` with a bare symbol.
    assert(STI.hasFeature(RISCV::FeatureVendorXqcili) &&
           "C.LI is only relaxable with Xqcili");
    Res.setOpcode(getRelaxedOpcode(Inst.getOpcode(), Inst.getOperands(), STI));
    Res.addOperand(Inst.getOperand(0));
    Res.addOperand(Inst.getOperand(1));
    break;
  }
  case RISCV::BEQ:
  case RISCV::BNE:
  case RISCV::BLT:
  case RISCV::BGE:
  case RISCV::BLTU:
  case RISCV::BGEU:
  case RISCV::QC_BEQI:
  case RISCV::QC_BNEI:
  case RISCV::QC_BLTI:
  case RISCV::QC_BGEI:
  case RISCV::QC_BLTUI:
  case RISCV::QC_BGEUI:
  case RISCV::QC_E_BEQI:
  case RISCV::QC_E_BNEI:
  case RISCV::QC_E_BLTI:
  case RISCV::QC_E_BGEI:
  case RISCV::QC_E_BLTUI:
  case RISCV::QC_E_BGEUI:
    Res.setOpcode(getRelaxedOpcode(Inst.getOpcode(), Inst.getOperands(), STI));
    Res.addOperand(Inst.getOperand(0));
    Res.addOperand(Inst.getOperand(1));
    Res.addOperand(Inst.getOperand(2));
    break;
  }
  Inst = std::move(Res);
}

// Check if an R_RISCV_ALIGN relocation is needed for an alignment directive.
// If conditions are met, compute the padding size and create a fixup encoding
// the padding size in the addend.
bool RISCVAsmBackend::relaxAlign(MCFragment &F, unsigned &Size) {
  // Use default handling unless linker relaxation is enabled and the alignment
  // is larger than the nop size.
  const MCSubtargetInfo *STI = F.getSubtargetInfo();
  if (!STI->hasFeature(RISCV::FeatureRelax))
    return false;
  unsigned MinNopLen = STI->hasFeature(RISCV::FeatureStdExtZca) ? 2 : 4;
  if (F.getAlignment() <= MinNopLen)
    return false;

  Size = F.getAlignment().value() - MinNopLen;
  auto *Expr = MCConstantExpr::create(Size, getContext());
  MCFixup Fixup =
      MCFixup::create(0, Expr, FirstLiteralRelocationKind + ELF::R_RISCV_ALIGN);
  F.setVarFixups({Fixup});
  F.setLinkerRelaxable();
  F.getParent()->setLinkerRelaxable();
  return true;
}

bool RISCVAsmBackend::relaxDwarfLineAddr(MCFragment &F,
                                         bool &WasRelaxed) const {
  MCContext &C = getContext();

  int64_t LineDelta = F.getDwarfLineDelta();
  const MCExpr &AddrDelta = F.getDwarfAddrDelta();
  SmallVector<MCFixup, 1> Fixups;
  size_t OldSize = F.getVarSize();

  int64_t Value;
  [[maybe_unused]] bool IsAbsolute =
      AddrDelta.evaluateKnownAbsolute(Value, *Asm);
  assert(IsAbsolute && "CFA with invalid expression");

  Fixups.clear();
  SmallVector<char> Data;
  raw_svector_ostream OS(Data);

  // INT64_MAX is a signal that this is actually a DW_LNE_end_sequence.
  if (LineDelta != INT64_MAX) {
    OS << uint8_t(dwarf::DW_LNS_advance_line);
    encodeSLEB128(LineDelta, OS);
  }

  unsigned Offset;
  std::pair<MCFixupKind, MCFixupKind> Fixup;

  // According to the DWARF specification, the `DW_LNS_fixed_advance_pc` opcode
  // takes a single unsigned half (unencoded) operand. The maximum encodable
  // value is therefore 65535.  Set a conservative upper bound for relaxation.
  if (Value > 60000) {
    unsigned PtrSize = C.getAsmInfo()->getCodePointerSize();

    OS << uint8_t(dwarf::DW_LNS_extended_op);
    encodeULEB128(PtrSize + 1, OS);

    OS << uint8_t(dwarf::DW_LNE_set_address);
    Offset = OS.tell();
    assert((PtrSize == 4 || PtrSize == 8) && "Unexpected pointer size");
    Fixup = RISCV::getRelocPairForSize(PtrSize);
    OS.write_zeros(PtrSize);
  } else {
    OS << uint8_t(dwarf::DW_LNS_fixed_advance_pc);
    Offset = OS.tell();
    Fixup = RISCV::getRelocPairForSize(2);
    support::endian::write<uint16_t>(OS, 0, llvm::endianness::little);
  }

  const MCBinaryExpr &MBE = cast<MCBinaryExpr>(AddrDelta);
  Fixups.push_back(MCFixup::create(Offset, MBE.getLHS(), std::get<0>(Fixup)));
  Fixups.push_back(MCFixup::create(Offset, MBE.getRHS(), std::get<1>(Fixup)));

  if (LineDelta == INT64_MAX) {
    OS << uint8_t(dwarf::DW_LNS_extended_op);
    OS << uint8_t(1);
    OS << uint8_t(dwarf::DW_LNE_end_sequence);
  } else {
    OS << uint8_t(dwarf::DW_LNS_copy);
  }

  F.setVarContents(Data);
  F.setVarFixups(Fixups);
  WasRelaxed = OldSize != Data.size();
  return true;
}

bool RISCVAsmBackend::relaxDwarfCFA(MCFragment &F, bool &WasRelaxed) const {
  const MCExpr &AddrDelta = F.getDwarfAddrDelta();
  SmallVector<MCFixup, 2> Fixups;
  size_t OldSize = F.getVarSize();

  int64_t Value;
  if (AddrDelta.evaluateAsAbsolute(Value, *Asm))
    return false;
  [[maybe_unused]] bool IsAbsolute =
      AddrDelta.evaluateKnownAbsolute(Value, *Asm);
  assert(IsAbsolute && "CFA with invalid expression");

  assert(getContext().getAsmInfo()->getMinInstAlignment() == 1 &&
         "expected 1-byte alignment");
  if (Value == 0) {
    F.clearVarContents();
    F.clearVarFixups();
    WasRelaxed = OldSize != 0;
    return true;
  }

  auto AddFixups = [&Fixups, &AddrDelta](unsigned Offset,
                                         std::pair<unsigned, unsigned> Fixup) {
    const MCBinaryExpr &MBE = cast<MCBinaryExpr>(AddrDelta);
    Fixups.push_back(MCFixup::create(Offset, MBE.getLHS(), std::get<0>(Fixup)));
    Fixups.push_back(MCFixup::create(Offset, MBE.getRHS(), std::get<1>(Fixup)));
  };

  SmallVector<char, 8> Data;
  raw_svector_ostream OS(Data);
  if (isUIntN(6, Value)) {
    OS << uint8_t(dwarf::DW_CFA_advance_loc);
    AddFixups(0, {ELF::R_RISCV_SET6, ELF::R_RISCV_SUB6});
  } else if (isUInt<8>(Value)) {
    OS << uint8_t(dwarf::DW_CFA_advance_loc1);
    support::endian::write<uint8_t>(OS, 0, llvm::endianness::little);
    AddFixups(1, {ELF::R_RISCV_SET8, ELF::R_RISCV_SUB8});
  } else if (isUInt<16>(Value)) {
    OS << uint8_t(dwarf::DW_CFA_advance_loc2);
    support::endian::write<uint16_t>(OS, 0, llvm::endianness::little);
    AddFixups(1, {ELF::R_RISCV_SET16, ELF::R_RISCV_SUB16});
  } else if (isUInt<32>(Value)) {
    OS << uint8_t(dwarf::DW_CFA_advance_loc4);
    support::endian::write<uint32_t>(OS, 0, llvm::endianness::little);
    AddFixups(1, {ELF::R_RISCV_SET32, ELF::R_RISCV_SUB32});
  } else {
    llvm_unreachable("unsupported CFA encoding");
  }
  F.setVarContents(Data);
  F.setVarFixups(Fixups);

  WasRelaxed = OldSize != Data.size();
  return true;
}

std::pair<bool, bool> RISCVAsmBackend::relaxLEB128(MCFragment &LF,
                                                   int64_t &Value) const {
  if (LF.isLEBSigned())
    return std::make_pair(false, false);
  const MCExpr &Expr = LF.getLEBValue();
  if (ULEB128Reloc) {
    LF.setVarFixups({MCFixup::create(0, &Expr, FK_Data_leb128)});
  }
  return std::make_pair(Expr.evaluateKnownAbsolute(Value, *Asm), false);
}

bool RISCVAsmBackend::mayNeedRelaxation(unsigned Opcode,
                                        ArrayRef<MCOperand> Operands,
                                        const MCSubtargetInfo &STI) const {
  // This function has access to two STIs, the member of the AsmBackend, and the
  // one passed as an argument. The latter is more specific, so we query it for
  // specific features.
  if (STI.hasFeature(RISCV::FeatureExactAssembly))
    return false;

  return getRelaxedOpcode(Opcode, Operands, STI) != Opcode;
}

bool RISCVAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                   const MCSubtargetInfo *STI) const {
  // We mostly follow binutils' convention here: align to even boundary with a
  // 0-fill padding.  We emit up to 1 2-byte nop, though we use c.nop if RVC is
  // enabled or 0-fill otherwise.  The remainder is now padded with 4-byte nops.

  // Instructions always are at even addresses.  We must be in a data area or
  // be unaligned due to some other reason.
  if (Count % 2) {
    OS.write("\0", 1);
    Count -= 1;
  }

  if (Count % 4 == 2) {
    // The canonical nop with Zca is c.nop.
    OS.write(STI->hasFeature(RISCV::FeatureStdExtZca) ? "\x01\0" : "\0\0", 2);
    Count -= 2;
  }

  // The canonical nop on RISC-V is addi x0, x0, 0.
  for (; Count >= 4; Count -= 4)
    OS.write("\x13\0\0\0", 4);

  return true;
}

static uint64_t adjustFixupValue(const MCFixup &Fixup, uint64_t Value,
                                 MCContext &Ctx) {
  switch (Fixup.getKind()) {
  default:
    llvm_unreachable("Unknown fixup kind!");
  case FK_Data_1:
  case FK_Data_2:
  case FK_Data_4:
  case FK_Data_8:
  case FK_Data_leb128:
    return Value;
  case RISCV::fixup_riscv_lo12_i:
  case RISCV::fixup_riscv_pcrel_lo12_i:
    return Value & 0xfff;
  case RISCV::fixup_riscv_12_i:
    if (!isInt<12>(Value)) {
      Ctx.reportError(Fixup.getLoc(),
                      "operand must be a constant 12-bit integer");
    }
    return Value & 0xfff;
  case RISCV::fixup_riscv_lo12_s:
  case RISCV::fixup_riscv_pcrel_lo12_s:
    return (((Value >> 5) & 0x7f) << 25) | ((Value & 0x1f) << 7);
  case RISCV::fixup_riscv_hi20:
  case RISCV::fixup_riscv_pcrel_hi20:
    // Add 1 if bit 11 is 1, to compensate for low 12 bits being negative.
    return ((Value + 0x800) >> 12) & 0xfffff;
  case RISCV::fixup_riscv_jal: {
    if (!isInt<21>(Value))
      Ctx.reportError(Fixup.getLoc(), "fixup value out of range");
    if (Value & 0x1)
      Ctx.reportError(Fixup.getLoc(), "fixup value must be 2-byte aligned");
    // Need to produce imm[19|10:1|11|19:12] from the 21-bit Value.
    unsigned Sbit = (Value >> 20) & 0x1;
    unsigned Hi8 = (Value >> 12) & 0xff;
    unsigned Mid1 = (Value >> 11) & 0x1;
    unsigned Lo10 = (Value >> 1) & 0x3ff;
    // Inst{31} = Sbit;
    // Inst{30-21} = Lo10;
    // Inst{20} = Mid1;
    // Inst{19-12} = Hi8;
    Value = (Sbit << 19) | (Lo10 << 9) | (Mid1 << 8) | Hi8;
    return Value;
  }
  case RISCV::fixup_riscv_qc_e_branch:
  case RISCV::fixup_riscv_branch: {
    if (!isInt<13>(Value))
      Ctx.reportError(Fixup.getLoc(), "fixup value out of range");
    if (Value & 0x1)
      Ctx.reportError(Fixup.getLoc(), "fixup value must be 2-byte aligned");
    // Need to extract imm[12], imm[10:5], imm[4:1], imm[11] from the 13-bit
    // Value.
    unsigned Sbit = (Value >> 12) & 0x1;
    unsigned Hi1 = (Value >> 11) & 0x1;
    unsigned Mid6 = (Value >> 5) & 0x3f;
    unsigned Lo4 = (Value >> 1) & 0xf;
    // Inst{31} = Sbit;
    // Inst{30-25} = Mid6;
    // Inst{11-8} = Lo4;
    // Inst{7} = Hi1;
    Value = (Sbit << 31) | (Mid6 << 25) | (Lo4 << 8) | (Hi1 << 7);
    return Value;
  }
  case RISCV::fixup_riscv_call:
  case RISCV::fixup_riscv_call_plt: {
    // Jalr will add UpperImm with the sign-extended 12-bit LowerImm,
    // we need to add 0x800ULL before extract upper bits to reflect the
    // effect of the sign extension.
    uint64_t UpperImm = (Value + 0x800ULL) & 0xfffff000ULL;
    uint64_t LowerImm = Value & 0xfffULL;
    return UpperImm | ((LowerImm << 20) << 32);
  }
  case RISCV::fixup_riscv_rvc_jump: {
    if (!isInt<12>(Value))
      Ctx.reportError(Fixup.getLoc(), "fixup value out of range");
    // Need to produce offset[11|4|9:8|10|6|7|3:1|5] from the 11-bit Value.
    unsigned Bit11  = (Value >> 11) & 0x1;
    unsigned Bit4   = (Value >> 4) & 0x1;
    unsigned Bit9_8 = (Value >> 8) & 0x3;
    unsigned Bit10  = (Value >> 10) & 0x1;
    unsigned Bit6   = (Value >> 6) & 0x1;
    unsigned Bit7   = (Value >> 7) & 0x1;
    unsigned Bit3_1 = (Value >> 1) & 0x7;
    unsigned Bit5   = (Value >> 5) & 0x1;
    Value = (Bit11 << 10) | (Bit4 << 9) | (Bit9_8 << 7) | (Bit10 << 6) |
            (Bit6 << 5) | (Bit7 << 4) | (Bit3_1 << 1) | Bit5;
    return Value;
  }
  case RISCV::fixup_riscv_rvc_branch: {
    if (!isInt<9>(Value))
      Ctx.reportError(Fixup.getLoc(), "fixup value out of range");
    // Need to produce offset[8|4:3], [reg 3 bit], offset[7:6|2:1|5]
    unsigned Bit8   = (Value >> 8) & 0x1;
    unsigned Bit7_6 = (Value >> 6) & 0x3;
    unsigned Bit5   = (Value >> 5) & 0x1;
    unsigned Bit4_3 = (Value >> 3) & 0x3;
    unsigned Bit2_1 = (Value >> 1) & 0x3;
    Value = (Bit8 << 12) | (Bit4_3 << 10) | (Bit7_6 << 5) | (Bit2_1 << 3) |
            (Bit5 << 2);
    return Value;
  }
  case RISCV::fixup_riscv_rvc_imm: {
    if (!isInt<6>(Value))
      Ctx.reportError(Fixup.getLoc(), "fixup value out of range");
    unsigned Bit5 = (Value >> 5) & 0x1;
    unsigned Bit4_0 = Value & 0x1f;
    Value = (Bit5 << 12) | (Bit4_0 << 2);
    return Value;
  }
  case RISCV::fixup_riscv_qc_e_32: {
    if (!isInt<32>(Value))
      Ctx.reportError(Fixup.getLoc(), "fixup value out of range");
    return Value & 0xffffffffu;
  }
  case RISCV::fixup_riscv_qc_abs20_u: {
    if (!isInt<20>(Value))
      Ctx.reportError(Fixup.getLoc(), "fixup value out of range");
    unsigned Bit19 = (Value >> 19) & 0x1;
    unsigned Bit14_0 = Value & 0x7fff;
    unsigned Bit18_15 = (Value >> 15) & 0xf;
    Value = (Bit19 << 31) | (Bit14_0 << 16) | (Bit18_15 << 12);
    return Value;
  }
  case RISCV::fixup_riscv_qc_e_call_plt: {
    if (!isInt<32>(Value))
      Ctx.reportError(Fixup.getLoc(), "fixup value out of range");
    if (Value & 0x1)
      Ctx.reportError(Fixup.getLoc(), "fixup value must be 2-byte aligned");
    uint64_t Bit31_16 = (Value >> 16) & 0xffff;
    uint64_t Bit12 = (Value >> 12) & 0x1;
    uint64_t Bit10_5 = (Value >> 5) & 0x3f;
    uint64_t Bit15_13 = (Value >> 13) & 0x7;
    uint64_t Bit4_1 = (Value >> 1) & 0xf;
    uint64_t Bit11 = (Value >> 11) & 0x1;
    Value = (Bit31_16 << 32ull) | (Bit12 << 31) | (Bit10_5 << 25) |
            (Bit15_13 << 17) | (Bit4_1 << 8) | (Bit11 << 7);
    return Value;
  }
  case RISCV::fixup_riscv_nds_branch_10: {
    if (!isInt<11>(Value))
      Ctx.reportError(Fixup.getLoc(), "fixup value out of range");
    if (Value & 0x1)
      Ctx.reportError(Fixup.getLoc(), "fixup value must be 2-byte aligned");
    // Need to extract imm[10], imm[9:5], imm[4:1] from the 11-bit Value.
    unsigned Sbit = (Value >> 10) & 0x1;
    unsigned Hi5 = (Value >> 5) & 0x1f;
    unsigned Lo4 = (Value >> 1) & 0xf;
    // Inst{31} = Sbit;
    // Inst{29-25} = Hi5;
    // Inst{11-8} = Lo4;
    Value = (Sbit << 31) | (Hi5 << 25) | (Lo4 << 8);
    return Value;
  }
  }
}

bool RISCVAsmBackend::isPCRelFixupResolved(const MCSymbol *SymA,
                                           const MCFragment &F) {
  // If the section does not contain linker-relaxable fragments, PC-relative
  // fixups can be resolved.
  if (!F.getParent()->isLinkerRelaxable())
    return true;

  // Otherwise, check if the offset between the symbol and fragment is fully
  // resolved, unaffected by linker-relaxable fragments (e.g. instructions or
  // offset-affected FT_Align fragments). Complements the generic
  // isSymbolRefDifferenceFullyResolvedImpl.
  if (!PCRelTemp)
    PCRelTemp = getContext().createTempSymbol();
  PCRelTemp->setFragment(const_cast<MCFragment *>(&F));
  MCValue Res;
  MCExpr::evaluateSymbolicAdd(Asm, false, MCValue::get(SymA),
                              MCValue::get(nullptr, PCRelTemp), Res);
  return !Res.getSubSym();
}

// Get the corresponding PC-relative HI fixup that a S_PCREL_LO points to, and
// optionally the fragment containing it.
//
// \returns nullptr if this isn't a S_PCREL_LO pointing to a known PC-relative
// HI fixup.
static const MCFixup *getPCRelHiFixup(const MCSpecifierExpr &Expr,
                                      const MCFragment **DFOut) {
  MCValue AUIPCLoc;
  if (!Expr.getSubExpr()->evaluateAsRelocatable(AUIPCLoc, nullptr))
    return nullptr;

  const MCSymbol *AUIPCSymbol = AUIPCLoc.getAddSym();
  if (!AUIPCSymbol)
    return nullptr;
  const auto *DF = AUIPCSymbol->getFragment();
  if (!DF)
    return nullptr;

  uint64_t Offset = AUIPCSymbol->getOffset();
  if (DF->getContents().size() == Offset) {
    DF = DF->getNext();
    if (!DF)
      return nullptr;
    Offset = 0;
  }

  for (const MCFixup &F : DF->getFixups()) {
    if (F.getOffset() != Offset)
      continue;
    auto Kind = F.getKind();
    if (!mc::isRelocation(F.getKind())) {
      if (Kind == RISCV::fixup_riscv_pcrel_hi20) {
        *DFOut = DF;
        return &F;
      }
      break;
    }
    switch (Kind) {
    case ELF::R_RISCV_GOT_HI20:
    case ELF::R_RISCV_TLS_GOT_HI20:
    case ELF::R_RISCV_TLS_GD_HI20:
    case ELF::R_RISCV_TLSDESC_HI20:
      *DFOut = DF;
      return &F;
    }
  }

  return nullptr;
}

std::optional<bool> RISCVAsmBackend::evaluateFixup(const MCFragment &,
                                                   MCFixup &Fixup,
                                                   MCValue &Target,
                                                   uint64_t &Value) {
  const MCFixup *AUIPCFixup;
  const MCFragment *AUIPCDF;
  MCValue AUIPCTarget;
  switch (Fixup.getKind()) {
  default:
    // Use default handling for `Value` and `IsResolved`.
    return {};
  case RISCV::fixup_riscv_pcrel_lo12_i:
  case RISCV::fixup_riscv_pcrel_lo12_s: {
    AUIPCFixup =
        getPCRelHiFixup(cast<MCSpecifierExpr>(*Fixup.getValue()), &AUIPCDF);
    if (!AUIPCFixup) {
      getContext().reportError(Fixup.getLoc(),
                               "could not find corresponding %pcrel_hi");
      return true;
    }

    // MCAssembler::evaluateFixup will emit an error for this case when it sees
    // the %pcrel_hi, so don't duplicate it when also seeing the %pcrel_lo.
    const MCExpr *AUIPCExpr = AUIPCFixup->getValue();
    if (!AUIPCExpr->evaluateAsRelocatable(AUIPCTarget, Asm))
      return true;
    break;
  }
  }

  if (!AUIPCTarget.getAddSym())
    return false;

  const MCSymbolELF &SA = cast<MCSymbolELF>(*AUIPCTarget.getAddSym());
  if (SA.isUndefined())
    return false;

  bool IsResolved = &SA.getSection() == AUIPCDF->getParent() &&
                    SA.getBinding() == ELF::STB_LOCAL &&
                    SA.getType() != ELF::STT_GNU_IFUNC;
  if (!IsResolved)
    return false;

  Value = Asm->getSymbolOffset(SA) + AUIPCTarget.getConstant();
  Value -= Asm->getFragmentOffset(*AUIPCDF) + AUIPCFixup->getOffset();

  return AUIPCFixup->getKind() == RISCV::fixup_riscv_pcrel_hi20 &&
         isPCRelFixupResolved(AUIPCTarget.getAddSym(), *AUIPCDF);
}

void RISCVAsmBackend::maybeAddVendorReloc(const MCFragment &F,
                                          const MCFixup &Fixup) {
  StringRef VendorIdentifier;
  switch (Fixup.getKind()) {
  default:
    // No Vendor Relocation Required.
    return;
  case RISCV::fixup_riscv_qc_e_branch:
  case RISCV::fixup_riscv_qc_abs20_u:
  case RISCV::fixup_riscv_qc_e_32:
  case RISCV::fixup_riscv_qc_e_call_plt:
    VendorIdentifier = "QUALCOMM";
    break;
  case RISCV::fixup_riscv_nds_branch_10:
    VendorIdentifier = "ANDES";
    break;
  }

  // Create a local symbol for the vendor relocation to reference. It's fine if
  // the symbol has the same name as an existing symbol.
  MCContext &Ctx = Asm->getContext();
  MCSymbol *VendorSymbol = Ctx.createLocalSymbol(VendorIdentifier);
  auto [It, Inserted] =
      VendorSymbols.try_emplace(VendorIdentifier, VendorSymbol);

  if (Inserted) {
    // Setup the just-created symbol
    VendorSymbol->setVariableValue(MCConstantExpr::create(0, Ctx));
    Asm->registerSymbol(*VendorSymbol);
  } else {
    // Fetch the existing symbol
    VendorSymbol = It->getValue();
  }

  MCFixup VendorFixup =
      MCFixup::create(Fixup.getOffset(), nullptr, ELF::R_RISCV_VENDOR);
  // Explicitly create MCValue rather than using an MCExpr and evaluating it so
  // that the absolute vendor symbol is not evaluated to constant 0.
  MCValue VendorTarget = MCValue::get(VendorSymbol);
  uint64_t VendorValue;
  Asm->getWriter().recordRelocation(F, VendorFixup, VendorTarget, VendorValue);
}

bool RISCVAsmBackend::addReloc(const MCFragment &F, const MCFixup &Fixup,
                               const MCValue &Target, uint64_t &FixedValue,
                               bool IsResolved) {
  uint64_t FixedValueA, FixedValueB;
  if (Target.getSubSym()) {
    assert(Target.getSpecifier() == 0 &&
           "relocatable SymA-SymB cannot have relocation specifier");
    unsigned TA = 0, TB = 0;
    switch (Fixup.getKind()) {
    case llvm::FK_Data_1:
      TA = ELF::R_RISCV_ADD8;
      TB = ELF::R_RISCV_SUB8;
      break;
    case llvm::FK_Data_2:
      TA = ELF::R_RISCV_ADD16;
      TB = ELF::R_RISCV_SUB16;
      break;
    case llvm::FK_Data_4:
      TA = ELF::R_RISCV_ADD32;
      TB = ELF::R_RISCV_SUB32;
      break;
    case llvm::FK_Data_8:
      TA = ELF::R_RISCV_ADD64;
      TB = ELF::R_RISCV_SUB64;
      break;
    case llvm::FK_Data_leb128:
      TA = ELF::R_RISCV_SET_ULEB128;
      TB = ELF::R_RISCV_SUB_ULEB128;
      break;
    default:
      llvm_unreachable("unsupported fixup size");
    }
    MCValue A = MCValue::get(Target.getAddSym(), nullptr, Target.getConstant());
    MCValue B = MCValue::get(Target.getSubSym());
    auto FA = MCFixup::create(Fixup.getOffset(), nullptr, TA);
    auto FB = MCFixup::create(Fixup.getOffset(), nullptr, TB);
    Asm->getWriter().recordRelocation(F, FA, A, FixedValueA);
    Asm->getWriter().recordRelocation(F, FB, B, FixedValueB);
    FixedValue = FixedValueA - FixedValueB;
    return false;
  }

  // If linker relaxation is enabled and supported by the current relocation,
  // generate a relocation and then append a RELAX.
  if (Fixup.isLinkerRelaxable())
    IsResolved = false;
  if (IsResolved && Fixup.isPCRel())
    IsResolved = isPCRelFixupResolved(Target.getAddSym(), F);

  if (!IsResolved) {
    // Some Fixups require a vendor relocation, record it (directly) before we
    // add the relocation.
    maybeAddVendorReloc(F, Fixup);

    Asm->getWriter().recordRelocation(F, Fixup, Target, FixedValue);
  }

  if (Fixup.isLinkerRelaxable()) {
    auto FA = MCFixup::create(Fixup.getOffset(), nullptr, ELF::R_RISCV_RELAX);
    Asm->getWriter().recordRelocation(F, FA, MCValue::get(nullptr),
                                      FixedValueA);
  }

  return false;
}

void RISCVAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                 const MCValue &Target,
                                 MutableArrayRef<char> Data, uint64_t Value,
                                 bool IsResolved) {
  IsResolved = addReloc(F, Fixup, Target, Value, IsResolved);
  MCFixupKind Kind = Fixup.getKind();
  if (mc::isRelocation(Kind))
    return;
  MCContext &Ctx = getContext();
  MCFixupKindInfo Info = getFixupKindInfo(Kind);
  if (!Value)
    return; // Doesn't change encoding.
  // Apply any target-specific value adjustments.
  Value = adjustFixupValue(Fixup, Value, Ctx);

  // Shift the value into position.
  Value <<= Info.TargetOffset;

  unsigned Offset = Fixup.getOffset();
  unsigned NumBytes = alignTo(Info.TargetSize + Info.TargetOffset, 8) / 8;

  assert(Offset + NumBytes <= Data.size() && "Invalid fixup offset!");

  // For each byte of the fragment that the fixup touches, mask in the
  // bits from the fixup value.
  for (unsigned i = 0; i != NumBytes; ++i) {
    Data[Offset + i] |= uint8_t((Value >> (i * 8)) & 0xff);
  }
}

std::unique_ptr<MCObjectTargetWriter>
RISCVAsmBackend::createObjectTargetWriter() const {
  return createRISCVELFObjectWriter(OSABI, Is64Bit);
}

MCAsmBackend *llvm::createRISCVAsmBackend(const Target &T,
                                          const MCSubtargetInfo &STI,
                                          const MCRegisterInfo &MRI,
                                          const MCTargetOptions &Options) {
  const Triple &TT = STI.getTargetTriple();
  uint8_t OSABI = MCELFObjectTargetWriter::getOSABI(TT.getOS());
  return new RISCVAsmBackend(STI, OSABI, TT.isArch64Bit(), Options);
}
