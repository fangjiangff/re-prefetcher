#include "X86.h"
#include "X86InstrBuilder.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineSSAUpdater.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/MC/MCSchedule.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include "MCTargetDesc/X86BaseInfo.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <utility>
#include <iostream>
#include <sstream>

using namespace llvm;

#define PASS_KEY "x86-miranalysize"

#define ARITHMETIC_INSTRUCTION_THRESHOLD 15
#define ARITHMETIC_INSTRUCTION_THRESHOLD_FOR_IMPLICIT 2

static cl::opt<bool> EnableAnalysize(
    "x86-mir-analyze",
    cl::desc("start MIR Analyzing"), cl::init(false),
    cl::Hidden);

namespace {
class X86MIRAnalyzePass : public MachineFunctionPass {

public:
  X86MIRAnalyzePass() : MachineFunctionPass(ID) { }
   StringRef getPassName() const override {
    return "X86 MIR Analyser";
  }
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Pass identification, replacement for typeid.
  static char ID;

private:
  const X86Subtarget *Subtarget = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  const X86InstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;


  MachineDominatorTree *MDT = nullptr;
  MachinePostDominatorTree *MPDT = nullptr;
  std::unique_ptr<MachineDominatorTree> OwnedMDT;
  std::unique_ptr<MachinePostDominatorTree> OwnedMPDT;

  // Helper function to check if an instruction is a CALL instruction to MEMCPY()
  bool isCallToMemcpy(const MachineInstr &MI) const;

  // Helper function to check if an instruction is a CALL instruction to MEMCPY()
  bool isInsideConditionalBranch(MachineInstr &MI, MachineDominatorTree *MDT, MachinePostDominatorTree *MPDT);

};
}

// Helper function to check if an instruction is a CMP instruction
static bool isCMP(unsigned Opcode) {
  switch (Opcode) {
  case X86::CMP8i8:      case X86::CMP16i16:    case X86::CMP32i32:    case X86::CMP64i32:
  case X86::CMP8mr:      case X86::CMP16mr:     case X86::CMP32mr:     case X86::CMP64mr:
  case X86::CMP8ri:      case X86::CMP16ri:     case X86::CMP32ri:     case X86::CMP64ri32:
  case X86::CMP8ri8:     case X86::CMP16ri8:    case X86::CMP32ri8:    case X86::CMP64ri8:
  case X86::CMP8rm:      case X86::CMP16rm:     case X86::CMP32rm:     case X86::CMP64rm:
  case X86::CMP8rr:      case X86::CMP16rr:     case X86::CMP32rr:     case X86::CMP64rr:
  case X86::CMP8rr_REV:  case X86::CMP16rr_REV: case X86::CMP32rr_REV: case X86::CMP64rr_REV:
    return true;
  default:
    return false;
  }
}

// Helper function to check if an instruction is a TEST instruction
static bool isTEST(unsigned Opcode) {
  switch (Opcode) {
  case X86::TEST8i8:     case X86::TEST16i16:   case X86::TEST32i32:   case X86::TEST64i32:
  case X86::TEST8mr:     case X86::TEST16mr:    case X86::TEST32mr:    case X86::TEST64mr:
  case X86::TEST8ri:     case X86::TEST16ri:    case X86::TEST32ri:    case X86::TEST64ri32:
  case X86::TEST8rr:     case X86::TEST16rr:    case X86::TEST32rr:    case X86::TEST64rr:
  case X86::TEST64mi32:
    return true;
  default:
    return false;
  }
}

// Helper function to check if an instruction is a TEST instruction
static bool isCMPorTEST(unsigned Opcode) {
  return isCMP(Opcode) || isTEST(Opcode);
}

// Helper function to check if an instruction is a CMOV instruction
static bool isCMOV(unsigned Opcode) {
  switch (Opcode) {
  case X86::CMOV16rr:    case X86::CMOV32rr:    case X86::CMOV64rr:
  case X86::CMOV16rm:    case X86::CMOV32rm:    case X86::CMOV64rm:
    return true;
  default:
    return false;
  }
}

// Helper function to check if an instruction modifies EFLAGS
static bool isModifiesEFLAGS(const MachineInstr &MI) {
  // Check implicit operands for EFLAGS definitions
  for (const auto &MO : MI.implicit_operands()) {
    if (MO.isReg() && MO.isDef() && MO.getReg() == X86::EFLAGS) {
      return true;
    }
  }
  return false;
}

// Helper function to check if an instruction uses EFLAGS
static bool isUsesEFLAGS(const MachineInstr &MI) {
  // Check implicit operands for EFLAGS definitions
  for (const auto &MO : MI.implicit_operands()) {
    if (MO.isReg() && MO.isUse() && MO.getReg() == X86::EFLAGS) {
      return true;
    }
  }
  return false;
}

// Helper function to check if an instruction is a floating-point arithmetic instruction
static bool isFLOAT(unsigned Opcode) {
  switch (Opcode) {
    // SSE Floating Point - Packed Single Precision (PS)
    case X86::ADDPSrr:   case X86::ADDPSrm:
    case X86::SUBPSrr:   case X86::SUBPSrm:
    case X86::MULPSrr:   case X86::MULPSrm:
    case X86::DIVPSrr:   case X86::DIVPSrm:

    // SSE Floating Point - Packed Double Precision (PD)
    case X86::ADDPDrr:   case X86::ADDPDrm:
    case X86::SUBPDrr:   case X86::SUBPDrm:
    case X86::MULPDrr:   case X86::MULPDrm:
    case X86::DIVPDrr:   case X86::DIVPDrm:

    // SSE Floating Point - Scalar Single Precision (SS)
    case X86::ADDSSrr:   case X86::ADDSSrm:   case X86::ADDSSrr_Int:   case X86::ADDSSrm_Int:
    case X86::SUBSSrr:   case X86::SUBSSrm:   case X86::SUBSSrr_Int:   case X86::SUBSSrm_Int:
    case X86::MULSSrr:   case X86::MULSSrm:   case X86::MULSSrr_Int:   case X86::MULSSrm_Int:
    case X86::DIVSSrr:   case X86::DIVSSrm:   case X86::DIVSSrr_Int:   case X86::DIVSSrm_Int:

    // SSE Floating Point - Scalar Double Precision (SD)
    case X86::ADDSDrr:   case X86::ADDSDrm:   case X86::ADDSDrr_Int:   case X86::ADDSDrm_Int:
    case X86::SUBSDrr:   case X86::SUBSDrm:   case X86::SUBSDrr_Int:   case X86::SUBSDrm_Int:
    case X86::MULSDrr:   case X86::MULSDrm:   case X86::MULSDrr_Int:   case X86::MULSDrm_Int:
    case X86::DIVSDrr:   case X86::DIVSDrm:   case X86::DIVSDrr_Int:   case X86::DIVSDrm_Int:

    // SQRT - Square Root - Packed Single Precision (PS)
    case X86::SQRTPSr:   case X86::SQRTPSm:

    // SQRT - Square Root - Packed Double Precision (PD)
    case X86::SQRTPDr:   case X86::SQRTPDm:

    // SQRT - Square Root - Scalar Single Precision (SS)
    case X86::SQRTSSr:   case X86::SQRTSSm:   case X86::SQRTSSr_Int:   case X86::SQRTSSm_Int:

    // SQRT - Square Root - Scalar Double Precision (SD)
    case X86::SQRTSDr:   case X86::SQRTSDm:   case X86::SQRTSDr_Int:   case X86::SQRTSDm_Int:

    // x87 FPU - Square Root
    case X86::SQRT_Fp32: case X86::SQRT_Fp64: case X86::SQRT_Fp80:
      return true;
    default:
      return false;
  }
}

// Helper function to check if an instruction is an arithmetic instruction
static bool isARITHMETIC(unsigned Opcode) {
  switch (Opcode) {
    // ADD - Addition
    case X86::ADD8ri:    case X86::ADD16ri:    case X86::ADD32ri:    case X86::ADD64ri32:
    case X86::ADD8rr:    case X86::ADD16rr:    case X86::ADD32rr:    case X86::ADD64rr:
    case X86::ADD8rm:    case X86::ADD16rm:    case X86::ADD32rm:    case X86::ADD64rm:
    case X86::ADD8mr:    case X86::ADD16mr:    case X86::ADD32mr:    case X86::ADD64mr:
    case X86::ADD8mi:    case X86::ADD16mi:    case X86::ADD32mi:    case X86::ADD64mi32:
    case X86::ADD16ri8:  case X86::ADD32ri8:   case X86::ADD64ri8:
    case X86::ADD16mi8:  case X86::ADD32mi8:   case X86::ADD64mi8:

    // SUB - Subtraction
    case X86::SUB8ri:    case X86::SUB16ri:    case X86::SUB32ri:    case X86::SUB64ri32:
    case X86::SUB8rr:    case X86::SUB16rr:    case X86::SUB32rr:    case X86::SUB64rr:
    case X86::SUB8rm:    case X86::SUB16rm:    case X86::SUB32rm:    case X86::SUB64rm:
    case X86::SUB8mr:    case X86::SUB16mr:    case X86::SUB32mr:    case X86::SUB64mr:
    case X86::SUB8mi:    case X86::SUB16mi:    case X86::SUB32mi:    case X86::SUB64mi32:
    case X86::SUB16ri8:  case X86::SUB32ri8:   case X86::SUB64ri8:
    case X86::SUB16mi8:  case X86::SUB32mi8:   case X86::SUB64mi8:
    case X86::SUB8mi8:

    // MUL - Unsigned Multiplication
    case X86::MUL8r:     case X86::MUL16r:     case X86::MUL32r:     case X86::MUL64r:
    case X86::MUL8m:     case X86::MUL16m:     case X86::MUL32m:     case X86::MUL64m:

    // IMUL - Signed Multiplication
    case X86::IMUL8r:    case X86::IMUL16r:    case X86::IMUL32r:    case X86::IMUL64r:
    case X86::IMUL8m:    case X86::IMUL16m:    case X86::IMUL32m:    case X86::IMUL64m:
    case X86::IMUL16rr:  case X86::IMUL32rr:   case X86::IMUL64rr:
    case X86::IMUL16rm:  case X86::IMUL32rm:   case X86::IMUL64rm:
    case X86::IMUL16rmi: case X86::IMUL32rmi:  case X86::IMUL64rmi32:
    case X86::IMUL16rri: case X86::IMUL32rri:  case X86::IMUL64rri32:
    case X86::IMUL16rmi8: case X86::IMUL32rmi8: case X86::IMUL64rmi8:
    case X86::IMUL16rri8: case X86::IMUL32rri8: case X86::IMUL64rri8:

    // DIV - Unsigned Division
    case X86::DIV8r:     case X86::DIV16r:     case X86::DIV32r:     case X86::DIV64r:
    case X86::DIV8m:     case X86::DIV16m:     case X86::DIV32m:     case X86::DIV64m:

    // IDIV - Signed Division
    case X86::IDIV8r:    case X86::IDIV16r:    case X86::IDIV32r:    case X86::IDIV64r:
    case X86::IDIV8m:    case X86::IDIV16m:    case X86::IDIV32m:    case X86::IDIV64m:

    // SHL/SAL - Shift Left (Logical/Arithmetic)
    case X86::SHL8r1:    case X86::SHL16r1:    case X86::SHL32r1:    case X86::SHL64r1:
    case X86::SHL8rCL:   case X86::SHL16rCL:   case X86::SHL32rCL:   case X86::SHL64rCL:
    case X86::SHL8ri:    case X86::SHL16ri:    case X86::SHL32ri:    case X86::SHL64ri:
    case X86::SHL8m1:    case X86::SHL16m1:    case X86::SHL32m1:    case X86::SHL64m1:
    case X86::SHL8mCL:   case X86::SHL16mCL:   case X86::SHL32mCL:   case X86::SHL64mCL:
    case X86::SHL8mi:    case X86::SHL16mi:    case X86::SHL32mi:    case X86::SHL64mi:

    // SHR - Shift Right (Logical)
    case X86::SHR8r1:    case X86::SHR16r1:    case X86::SHR32r1:    case X86::SHR64r1:
    case X86::SHR8rCL:   case X86::SHR16rCL:   case X86::SHR32rCL:   case X86::SHR64rCL:
    case X86::SHR8ri:    case X86::SHR16ri:    case X86::SHR32ri:    case X86::SHR64ri:
    case X86::SHR8m1:    case X86::SHR16m1:    case X86::SHR32m1:    case X86::SHR64m1:
    case X86::SHR8mCL:   case X86::SHR16mCL:   case X86::SHR32mCL:   case X86::SHR64mCL:
    case X86::SHR8mi:    case X86::SHR16mi:    case X86::SHR32mi:    case X86::SHR64mi:

    // SAR - Shift Right (Arithmetic)
    case X86::SAR8r1:    case X86::SAR16r1:    case X86::SAR32r1:    case X86::SAR64r1:
    case X86::SAR8rCL:   case X86::SAR16rCL:   case X86::SAR32rCL:   case X86::SAR64rCL:
    case X86::SAR8ri:    case X86::SAR16ri:    case X86::SAR32ri:    case X86::SAR64ri:
    case X86::SAR8m1:    case X86::SAR16m1:    case X86::SAR32m1:    case X86::SAR64m1:
    case X86::SAR8mCL:   case X86::SAR16mCL:   case X86::SAR32mCL:   case X86::SAR64mCL:
    case X86::SAR8mi:    case X86::SAR16mi:    case X86::SAR32mi:    case X86::SAR64mi:

    // AND - Logical AND
    case X86::AND8rr:    case X86::AND16rr:    case X86::AND32rr:    case X86::AND64rr:
    case X86::AND8ri:    case X86::AND16ri:    case X86::AND32ri:    case X86::AND64ri32:
    case X86::AND8ri8:   case X86::AND16ri8:   case X86::AND32ri8:   case X86::AND64ri8:
    case X86::AND8rm:    case X86::AND16rm:    case X86::AND32rm:    case X86::AND64rm:
    case X86::AND8mr:    case X86::AND16mr:    case X86::AND32mr:    case X86::AND64mr:
    case X86::AND8mi:    case X86::AND16mi:    case X86::AND32mi:    case X86::AND64mi32:
    case X86::AND16mi8:  case X86::AND32mi8:   case X86::AND64mi8:

    // OR - Logical OR
    case X86::OR8rr:     case X86::OR16rr:     case X86::OR32rr:     case X86::OR64rr:
    case X86::OR8ri:     case X86::OR16ri:     case X86::OR32ri:     case X86::OR64ri32:
    case X86::OR8ri8:    case X86::OR16ri8:    case X86::OR32ri8:    case X86::OR64ri8:
    case X86::OR8rm:     case X86::OR16rm:     case X86::OR32rm:     case X86::OR64rm:
    case X86::OR8mr:     case X86::OR16mr:     case X86::OR32mr:     case X86::OR64mr:
    case X86::OR8mi:     case X86::OR16mi:     case X86::OR32mi:     case X86::OR64mi32:
    case X86::OR16mi8:   case X86::OR32mi8:    case X86::OR64mi8:

    // XOR - Logical XOR
    case X86::XOR8rr:    case X86::XOR16rr:    case X86::XOR32rr:    case X86::XOR64rr:
    case X86::XOR8ri:    case X86::XOR16ri:    case X86::XOR32ri:    case X86::XOR64ri32:
    case X86::XOR8ri8:   case X86::XOR16ri8:   case X86::XOR32ri8:   case X86::XOR64ri8:
    case X86::XOR8rm:    case X86::XOR16rm:    case X86::XOR32rm:    case X86::XOR64rm:
    case X86::XOR8mr:    case X86::XOR16mr:    case X86::XOR32mr:    case X86::XOR64mr:
    case X86::XOR8mi:    case X86::XOR16mi:    case X86::XOR32mi:    case X86::XOR64mi32:
    case X86::XOR16mi8:  case X86::XOR32mi8:   case X86::XOR64mi8:

      return true;

    default:
      // Also check for floating-point arithmetic instructions
      return isFLOAT(Opcode);
  }
}

// Helper function to check if an instruction is a REP_MOVS instruction
static bool isREPMOVS(unsigned Opcode) {
  switch (Opcode) {
    case X86::REP_MOVSB_64: case X86::REP_MOVSW_64: case X86::REP_MOVSD_64: case X86::REP_MOVSQ_64:
    case X86::REP_MOVSB_32: case X86::REP_MOVSW_32: case X86::REP_MOVSD_32: case X86::REP_MOVSQ_32:
      return true;
    default:
      return false;
  }
}

// Helper function to check if all explicit operands of an instruction are clean (not in taint list)
static bool isExplicitOperandClean(const MachineInstr &MI, const SmallDenseMap<unsigned, unsigned, 32>& taint_register_list) {
  for (const MachineOperand &MO : MI.explicit_operands()) {
    if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
      // Check if the register is in the taint list
        return false; // Operand is tainted
    }
  }
  return true; // All operands are clean
}

// Helper function to check if an instruction is a CALL instruction to MEMCPY()
bool X86MIRAnalyzePass::isCallToMemcpy(const MachineInstr &MI) const {

  if (!MI.isCall()) {
    return false;
  }

  for (const MachineOperand &MO : MI.operands()) {
            
    const char* targetName = nullptr;
      
      if (MO.isGlobal()) 
      {
        targetName = MO.getGlobal()->getName().data(); 
      }
      else if(MO.isSymbol())
      {
        targetName = MO.getSymbolName();
      }

      if (targetName && strcmp(targetName, "memcpy") == 0) {
        return true;
      }

  }

  return false;
}

// Check if an instruction is inside a conditional branch using dominator tree analysis
// Returns true if MI is in a branch like: if (cond) { MI; } or else { MI; }
bool X86MIRAnalyzePass::isInsideConditionalBranch(MachineInstr &MI, MachineDominatorTree *MDT, MachinePostDominatorTree *MPDT) {
  MachineBasicBlock *MBB = MI.getParent();
  MachineFunction *MF = MBB->getParent();

  // Iterate through all basic blocks in the function
  for (auto &BB : *MF) {
    // Skip if this block doesn't have multiple successors (not a branch point)
    if (BB.succ_size() <= 1) {
      continue;
    }

    // Check if this block ends with a conditional branch
    auto Term = BB.getFirstTerminator();
    if (Term == BB.end() || !Term->isConditionalBranch()) {
      continue;
    }

    // Key insight: If BB dominates MBB but does NOT post-dominate MBB,
    // then MBB is on only one branch path from BB (i.e., inside a conditional)

    // Check if BB dominates MBB (BB is on all paths from entry to MBB)
    if (!MDT->dominates(&BB, MBB)) {
      continue;
    }

    // Check if MBB does NOT post-dominate BB (MBB is NOT on all paths from BB to exit)
    // Key fix: If MBB post-dominates BB, then all branches from BB merge at MBB (convergence point)
    // If MBB does NOT post-dominate BB, then MBB is only on some branch paths from BB
    if (MPDT && !MPDT->dominates(MBB, &BB)) {
      // BB dominates MBB (BB is before MBB)
      // MBB does NOT post-dominate BB (MBB is not on all paths after BB)
      // Therefore, MI is inside one of BB's conditional branches
      return true;
    }
  }

  return false;
}

// Find a load on one side of a conditional branch before control flow merges.
static MachineInstr *findFirstLoadOnBranchPath(MachineBasicBlock *Start,
                                               MachineBasicBlock *BranchMBB,
                                               MachinePostDominatorTree *MPDT) {
  SmallVector<MachineBasicBlock *, 8> WorkList;
  SmallPtrSet<MachineBasicBlock *, 16> Visited;

  WorkList.push_back(Start);

  while (!WorkList.empty()) {
    MachineBasicBlock *MBB = WorkList.pop_back_val();
    if (!Visited.insert(MBB).second) {
      continue;
    }

    if (MPDT && MPDT->dominates(MBB, BranchMBB)) {
      continue;
    }

    for (MachineInstr &MI : *MBB) {
      if (MI.mayLoad() && !MI.isCall()) {
        return &MI;
      }
    }

    for (MachineBasicBlock *Succ : MBB->successors()) {
      if (!Visited.count(Succ)) {
        WorkList.push_back(Succ);
      }
    }
  }

  return nullptr;
}

// Find a store on one side of a conditional branch before control flow merges.
static MachineInstr *findFirstStoreOnBranchPath(MachineBasicBlock *Start,
                                                MachineBasicBlock *BranchMBB,
                                                MachinePostDominatorTree *MPDT) {
  SmallVector<MachineBasicBlock *, 8> WorkList;
  SmallPtrSet<MachineBasicBlock *, 16> Visited;

  WorkList.push_back(Start);

  while (!WorkList.empty()) {
    MachineBasicBlock *MBB = WorkList.pop_back_val();
    if (!Visited.insert(MBB).second) {
      continue;
    }

    if (MPDT && MPDT->dominates(MBB, BranchMBB)) {
      continue;
    }

    for (MachineInstr &MI : *MBB) {
      if (MI.mayStore() && !MI.isCall()) {
        return &MI;
      }
    }

    for (MachineBasicBlock *Succ : MBB->successors()) {
      if (!Visited.count(Succ)) {
        WorkList.push_back(Succ);
      }
    }
  }

  return nullptr;
}


static std::string getDedupKey(StringRef Type, const MachineInstr *PrimaryMI,
                               const MachineInstr *SecondaryMI = nullptr) {
  std::ostringstream OS;
  OS << Type.str();

  auto addMI = [&](const MachineInstr *MI) {
    OS << "|";
    if (!MI) {
      OS << "null";
      return;
    }

    OS << "BB" << MI->getParent()->getNumber();
    if (MI->getDebugLoc()) {
      if (DILocation *Loc = MI->getDebugLoc().get()) {
        OS << "|" << Loc->getFilename().str() << ":" << Loc->getLine();
        return;
      }
    }

    unsigned InstrIndex = 0;
    for (const MachineInstr &CurMI : *MI->getParent()) {
      if (&CurMI == MI)
        break;
      ++InstrIndex;
    }
    OS << "|idx" << InstrIndex;
  };

  addMI(PrimaryMI);
  addMI(SecondaryMI);
  return OS.str();
}

static bool markIfNew(SmallSet<std::string, 32> &Seen,
                      const std::string &Key) {
  if (Seen.count(Key))
    return false;
  Seen.insert(Key);
  return true;
}

// Check if there are

char X86MIRAnalyzePass::ID = 0;

void X86MIRAnalyzePass::getAnalysisUsage(AnalysisUsage &AU) const {
  
  AU.setPreservesCFG();
  MachineFunctionPass::getAnalysisUsage(AU);
  
}

// ##################### ENTRAL FUNCTION #####################
// Main function that runs the analysis on the MachineFunction
bool X86MIRAnalyzePass::runOnMachineFunction(MachineFunction &MF)
{

  if (!EnableAnalysize) {
    return false;
  }

  Subtarget = &MF.getSubtarget<X86Subtarget>();
  MRI = &MF.getRegInfo();
  TII = Subtarget->getInstrInfo();
  TRI = Subtarget->getRegisterInfo();

  // Get dominator tree and post-dominator tree for branch detection
  auto *MDTWrapper = getAnalysisIfAvailable<MachineDominatorTreeWrapperPass>();
  
  if (MDTWrapper) {
    MDT = &MDTWrapper->getDomTree();
  } else {
    OwnedMDT = std::make_unique<MachineDominatorTree>();
    OwnedMDT->recalculate(MF);
    MDT = OwnedMDT.get();
  }

  auto *MPDTWrapper = getAnalysisIfAvailable<MachinePostDominatorTreeWrapperPass>();

  if (MPDTWrapper) {
    MPDT = &MPDTWrapper->getPostDomTree();
  } else {
    OwnedMPDT = std::make_unique<MachinePostDominatorTree>();
    OwnedMPDT->recalculate(MF);
    MPDT = OwnedMPDT.get();
  }

  if (!MDT || !MPDT) {
    return false;
  }

  if (MF.begin() == MF.end()) {
    return false;
  }

  SmallSet<std::string, 32> SeenGadgets;


  // ###########################################################################
  // 
  // Gadget searching #1:
  //         for variable-time memory access gadgets
  // 
  // Parameters:
  //         @ taint_register_list: the set of registers, which are explicitly tainted from function liveins
  //         @ taint_value_list: the set of reigsters, which are tainted from the suspicious memory load/store values
  //         @ taint_storeOrload_MIs: the set of load/store instructions that use tainted registers
  // 
  // Pattern:
  //         ...
  //         
  //
  //         # spectre v2 type
  //
  //         call  func  // we assume func can be mispredicted to here
  //
  //         def func { 
  //             secret->taint_register_list; 
  //             load_or_store(dest, src=item in taint_register_list);
  //             dest->taint_value_list;
  //             arithmetic_instructions(dest, src=item in taint_value_list); 
  //             
  //             call/ret/jmp *;
  //        }
  //
  //        ...
  // 
  // ###########################################################################
  
  { // Gadget searching #1 scope
    
    SmallDenseMap<unsigned, unsigned, 32> taint_register_list;
    SmallDenseMap<unsigned, unsigned, 32> taint_value_list;

    SmallDenseMap<MachineInstr *, int> taint_loadOrstore_MIs;

    int livein_count = 0;


    for (auto &Li : MF.begin()->liveins()) { 
      
      // Step1: Track all registers propagated from the livein
      // After this loop, we should have all registers EXPLICITLY propagated from the Li @ taint_register_list
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {
          for (auto &MO : MI.explicit_operands()) {
            
            if (MO.isReg() && MO.isUse() && (MO.getReg() == Li.PhysReg)) {
              for (auto & MO_tmp : MI.defs() )
                if (MO_tmp.isReg()) taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
            } 
            
            else if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
              for (auto & MO_tmp : MI.defs() ) {
                if (MO_tmp.isReg()) taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
              }
            }

          }
        }
      }

      // Step2: Find all load/store instructions that use tainted registers
      // After this loop, we should have all load/store instructions that 
      //     1) use tainted registers 
      //     @ taint_storeOrload_MIs
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {

          if (!MI.mayLoadOrStore()) {
            continue;
          }
          
          for (auto &MO : MI.explicit_operands()) {
            if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg()) && !taint_loadOrstore_MIs.count(&MI)) {
              ++taint_loadOrstore_MIs[&MI];
            }
          }

        }
      }

      // Step3: For each load/store instruction found in Step2, track the destination register and propagate it in the same BB
      // Thus, we can find all registers tainted from the load/store value @ taint_value_list
      // Finally, we check and report how many arithmetic instructions use these tainted values
      for (auto &Entry : taint_loadOrstore_MIs) {
        MachineInstr *loadOrstore_MI = Entry.first;
        MachineBasicBlock *MBB = loadOrstore_MI->getParent();

        // ATTENTION plz: you should clear the taint_value_list for each load/store instruction
        taint_value_list.clear();

        // Extract load/store operands and mark them as tainted
        // Rule: propogate explicitly from src Reg -> dest Reg
        for (auto &MO : loadOrstore_MI->explicit_operands()) {
          if (MO.isReg() && MO.isDef()) {
            taint_value_list[MO.getReg()] = MO.getReg();
          }
        }

        // The second propagation: propagate the taint_value_list within the same basic block
        for (auto MI_Iter = std::next(loadOrstore_MI->getIterator()); MI_Iter != MBB->end(); ++MI_Iter) {
          
          // Check if any operand uses a tainted register
          // If yes, propagate taint to all dest registers
          for (auto &MO : MI_Iter->explicit_operands()) {
            if (MO.isReg() && MO.isUse() && taint_value_list.count(MO.getReg())) {
              // Propagate taint to all dest registers
              for (auto &MO_dest : MI_Iter->explicit_operands()) {
                if (MO_dest.isReg() && MO_dest.isDef()) {
                  taint_value_list[MO_dest.getReg()] = MO_dest.getReg();
                }
              }
              break;
            }
          }

        }
      

        int arithmetic_instruction_cnt = 0;
        // Track the last arithmetic instruction so we can find a following
        // control-flow instruction.
        MachineInstr *last_arithmetic_MI = nullptr;

        // After we have the taint_value_list, we then check how many arithmetic instructions EXPLICITLY used these tainted values
        for (auto MI_Iter = std::next(loadOrstore_MI->getIterator()); MI_Iter != MBB->end(); ++MI_Iter) {

          if (!isARITHMETIC(MI_Iter->getOpcode())) {
            continue;
          }

          for (auto &MO : MI_Iter->explicit_operands()) {
            if (MO.isReg() && MO.isUse() && taint_value_list.count(MO.getReg())) {
              ++arithmetic_instruction_cnt;

              last_arithmetic_MI = &*MI_Iter;

              break;
            }
          }

        }

        // Finally, check if the count exceeds the threshold
        // If yes, we found a potential gadget, then report it
        if (arithmetic_instruction_cnt >= ARITHMETIC_INSTRUCTION_THRESHOLD) {
          
          // Check whether a control-flow instruction follows the last arithmetic instruction.
          MachineInstr *found_call_MI = nullptr;
          if (last_arithmetic_MI) {
              for (auto CallIter = std::next(last_arithmetic_MI->getIterator()); CallIter != MBB->end(); ++CallIter) {
                  if (CallIter->isCall() || CallIter->isBranch() || CallIter->isReturn()) {
                      found_call_MI = &*CallIter;
                      break; // find
                  }
              }
          }

          if (found_call_MI != nullptr) {
            
            // Source location for the load/store instruction.
            std::string filename = "unknown";
            unsigned line = 0;
            if (loadOrstore_MI->getDebugLoc()) {
              const DebugLoc &DL = loadOrstore_MI->getDebugLoc();
              if (DILocation *Loc = DL.get()) {
                filename = Loc->getFilename().str();
                line = Loc->getLine();
              }
            }

            // Source location for the following control-flow instruction.
            std::string call_filename = "unknown";
            unsigned call_line = 0;
            if (found_call_MI->getDebugLoc()) {
              const DebugLoc &DL = found_call_MI->getDebugLoc();
              if (DILocation *Loc = DL.get()) {
                call_filename = Loc->getFilename().str();
                call_line = Loc->getLine();
              }
            }

            if (!markIfNew(SeenGadgets, getDedupKey("variable-time-memory", loadOrstore_MI, found_call_MI))) {
              continue;
            }

            std::cout << " Variable-time memory access Gadgets Found in ---->  "
                << MF.getName().str()
                << " (file: " << filename << ":" << line << ")"
                << " livein " << livein_count
                << ", load-or-store at BB#" << loadOrstore_MI->getParent()->getNumber()
                << ", arithmetic_instructions_cnt: " << arithmetic_instruction_cnt
                << " (threshold: " << ARITHMETIC_INSTRUCTION_THRESHOLD << ")"
                << " FOLLOWED BY CALL at " << call_filename << ":" << call_line
                << std::endl;
            goto end_search1;
          }
        }
      }
  
      end_search1:
        taint_register_list.clear();
        taint_value_list.clear();
        taint_loadOrstore_MIs.clear();
        ++livein_count;
    }
  }

  // ###########################################################################
  // 
  // Gadget searching #2:
  //         for REP-MOVSB gadgets
  // 
  //         @ taint_register_list: the set of registers, which are explicitly tainted from function liveins
  //         @ gpr_taint_eflags_MIs: the set of instructions that use tainted registers to modify EFLAGS implicitly
  //         @ eflags_propagation_register_list: the set of instructions that propagate EFLAGS implicitly
  //         @ taint_from_implicit_register_list: all registers tainted from the implicit path
  //         @ taint_memcpy_MIs: the set of memcpy calls that use implicitly tainted registers for len  // 
  // Pattern:
  //         ...
  //
  //         # spectre v2 type
  //
  //         call  func  // we assume func can be mispredicted to here
  //
  //         def func { 
  //             secret->taint_register_list;
  //             secret -> (explicitly) OP_define_eflags (e.g. cmp/test/add/sub/...)
  //             OP_define_eflags dest -> eflags_propagation_register_list
  //             eflags_propagation_register_list -> (explicitly) OP_use_eflags (e.g. cmov/set/adc/sbb/...)
  //             
  //             ...
  //
  //             memcpy(src, dest, len=item in taint_from_implicit_register_list);
  //
  //             call/ret/jmp *;
  //         }
  //
  //        ...
  // 
  // ###########################################################################
  { // Gadget searching #2 scope

    SmallDenseMap<unsigned, unsigned, 32> taint_register_list;
    SmallDenseMap<MachineInstr *, int> gpr_taint_eflags_MIs;
    SmallDenseMap<unsigned, unsigned, 32> eflags_propagation_register_list;
    SmallDenseMap<unsigned, unsigned, 32> taint_from_implicit_register_list;
    SmallDenseMap<MachineInstr *, int> taint_memcpy_MIs;

    int livein_count = 0;

    for (auto &Li : MF.begin()->liveins()) {
      
      
      // Step1: Track all registers propagated from the livein
      // After this loop, we should have all registers IMPLICIT propagated from the Li @ taint_register_list
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {
          for (auto &MO : MI.explicit_operands()) {
            
            if (MO.isReg() && MO.isUse() && (MO.getReg() == Li.PhysReg)) {
              for (auto & MO_tmp : MI.defs() )
                if (MO_tmp.isReg()) taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
            } 
            
            else if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
              for (auto & MO_tmp : MI.defs() ) {
                if (MO_tmp.isReg()) taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
              }
            }

          }
        }
      }


      // Step2: Find all instructions that use tainted registers
      // After this loop, we should have all instructions that 
      //     1) use tainted registers
      //     2) define EFLAGS implicitly
      //     @ gpr_taint_eflags_MIs
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {

          if (!isModifiesEFLAGS(MI)) {
            continue;
          }

          for (auto &MO : MI.explicit_operands()) {
            if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg()))
            {
              gpr_taint_eflags_MIs[&MI]++;
              break;
            }
          }

        }
      }


      // Step3: For each instruction found in Step2
      // We need to check whether the tainted EFLAGS will be propagated to other instructions implicitly
      // This required the new instructions use 1) clean explicit operands and 2) dirty EFLAGS operand
      // If the eflags is modified by another instruction, we stop the propagation
      for (auto &Entry : gpr_taint_eflags_MIs) {
        
        MachineInstr *_MI = Entry.first;
        MachineBasicBlock *MBB = _MI->getParent();

        eflags_propagation_register_list.clear();
        taint_from_implicit_register_list.clear();
        taint_memcpy_MIs.clear();
        

        // First, check whether the instruction use EFLAGS: No, skipped; Yes, and if op is clean, record it
        // This loop finds the *first* level of implicitly tainted registers (e.g., dest of CMOV)
        for (auto MI_Iter = std::next(_MI->getIterator()); MI_Iter != MBB->end(); ++MI_Iter) {
          MachineInstr &CurrentMI = *MI_Iter;

          // Ignore instructions that neither use nor modify EFLAGS
          if (!isUsesEFLAGS(CurrentMI) && !isModifiesEFLAGS(CurrentMI)) {
            continue;
          }

          // If EFLAGS is used (e.g. cmovcc/setcc/adc/sbb)
          // Then the destination register is implicitly tainted
          if (isUsesEFLAGS(CurrentMI)) {
            for (auto &MO_explicit : CurrentMI.explicit_operands()) {
              if (MO_explicit.isReg() && MO_explicit.isDef()) {
                // Record the dest register
                eflags_propagation_register_list[MO_explicit.getReg()] = MO_explicit.getReg();
              }
            }
          }

          // If EFLAGS is modified, stop propagation
          // Any instruction that modifies EFLAGS (e.g., another ADD, SUB, CMP)
          // will overwrite the tainted EFLAGS value.
          if (isModifiesEFLAGS(CurrentMI)) {
            break;
          }
        }

        // Now, we record all dest registers that are implicitly tainted from EFLAGS
        if (eflags_propagation_register_list.empty()) {
          continue;
        } 

        // Next, we traverse again and 1) propagate the tainted registers @ taint_from_implicit_register_list
        // This finds all registers *derived* from the first-level implicitly tainted ones
        for (auto MI_Iter = std::next(_MI->getIterator()); MI_Iter != MBB->end(); ++MI_Iter) {
          MachineInstr &CurrentMI = *MI_Iter;

          for (auto &MO : CurrentMI.explicit_operands()) {
            if (MO.isReg() && MO.isUse() && (eflags_propagation_register_list.count(MO.getReg()) || taint_from_implicit_register_list.count(MO.getReg()))) {
                for (auto &MO_def : CurrentMI.defs()) {
                  if (MO_def.isReg()) {
                    taint_from_implicit_register_list[MO_def.getReg()] = MO_def.getReg();
                  }
                }
            }
          }

        }

        // Now, we get a list of taint_from_implicit_register_list, which records all the registers tainted from EFLAGS
        // Then, we traverse again and try to find memcpy instructions that use these tainted registers @ taint_memcpy_MIs
        for (auto MI_Iter = std::next(_MI->getIterator()); MI_Iter != MBB->end(); ++MI_Iter) {
          MachineInstr &CurrentMI = *MI_Iter;

          if (!isCallToMemcpy(CurrentMI)) {
            continue;
          }
          
          bool is_rdx_tainted = false;
          // Check implicit operands for %RDX (the 'len' parameter)
          for (MachineOperand &MO : CurrentMI.implicit_operands()) {
            if (MO.isReg() && MO.isUse() && MO.getReg() == X86::RDX) {
                // Check if RDX is in *either* the original EFLAGS-derived list or the propagated list
                if (taint_from_implicit_register_list.count(X86::RDX) || eflags_propagation_register_list.count(X86::RDX)) {
                  is_rdx_tainted = true;
                  break;
                }
            }
          }

          if (is_rdx_tainted) {
              ++taint_memcpy_MIs[&CurrentMI];
              // We can break here since we found a tainted call in this path
              // Or continue to find all. Let's find all for this _MI.
          }
        }

        // Next, for each memcpy instruction found, report it.
        // We don't need to check for arithmetic instructions after it; the memcpy is the gadget.
        for (auto &Entry : taint_memcpy_MIs) {
          MachineInstr *memcpy_MI = Entry.first;
          MachineBasicBlock *CurrentMBB = memcpy_MI->getParent();

          MachineInstr *trigger_MI = nullptr;

          for (auto NextIter = std::next(memcpy_MI->getIterator()); NextIter != CurrentMBB->end(); ++NextIter) {
            MachineInstr &NextMI = *NextIter;

            if (NextMI.isCall() || NextMI.isReturn() || NextMI.isBranch()) {
              trigger_MI = &NextMI;
              break; 
            }
          }

          if (trigger_MI == nullptr) {
            continue;
          }

          std::string memcpy_filename = "unknown";
          unsigned memcpy_line = 0;
          if (memcpy_MI->getDebugLoc()) {
            if (DILocation *Loc = memcpy_MI->getDebugLoc().get()) {
              memcpy_filename = Loc->getFilename().str();
              memcpy_line = Loc->getLine();
            }
          }

          std::string trigger_filename = "unknown";
          unsigned trigger_line = 0;
          if (trigger_MI->getDebugLoc()) {
            if (DILocation *Loc = trigger_MI->getDebugLoc().get()) {
              trigger_filename = Loc->getFilename().str();
              trigger_line = Loc->getLine();
            }
          }

          if (!markIfNew(SeenGadgets, getDedupKey("rep-movsb", memcpy_MI, trigger_MI))) {
            continue;
          }

          std::cout << " REP-MOVSB Gadgets Found in ---->  "
              << MF.getName().str()
              << "\n\t[+] Memcpy at: " << memcpy_filename << ":" << memcpy_line 
              << " (BB#" << memcpy_MI->getParent()->getNumber() << ")"
              << "\n\t[+] Followed by Control-Flow (Call/Ret/Jmp) at: " << trigger_filename << ":" << trigger_line
              << "\n\t[+] Taint Info: livein " << livein_count << ", implicitly tainted len(%RDX)"
              << std::endl;
              
          goto end_search2; // Found a gadget, stop searching this function
        }
      }

      end_search2:
        taint_register_list.clear();
        gpr_taint_eflags_MIs.clear();
        eflags_propagation_register_list.clear();
        taint_from_implicit_register_list.clear();
        taint_memcpy_MIs.clear();
        ++livein_count;
    }
  }

  // ###########################################################################
  //
  // Gadget searching #3:
  //         for control-flow load gadgets
  //
  // Pattern:
  //         secret -> taint_register_list
  //         tainted register -> OP_define_eflags (e.g. cmp/test/add/sub/...)
  //         OP_define_eflags -> conditional branch (Jcc)
  //         taken/fallthrough branch region contains load instruction(s)
  //
  // This matches the vulnerable pattern:
  //
  //         if (secret-dependent condition) {
  //             load(...);
  //         } else {
  //             load(...);
  //         }
  //
  // ###########################################################################
  { // Gadget searching #3 scope

    SmallDenseMap<unsigned, unsigned, 32> taint_register_list;

    int livein_count = 0;

    for (auto &Li : MF.begin()->liveins()) {

      taint_register_list[Li.PhysReg] = Li.PhysReg;

      // Step1: Track all registers explicitly propagated from the livein.
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {
          for (auto &MO : MI.explicit_operands()) {

            if (MO.isReg() && MO.isUse() && (MO.getReg() == Li.PhysReg)) {
              for (auto &MO_tmp : MI.defs()) {
                if (MO_tmp.isReg()) {
                  taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
                }
              }
            }

            else if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
              for (auto &MO_tmp : MI.defs()) {
                if (MO_tmp.isReg()) {
                  taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
                }
              }
            }
          }
        }
      }

      // Step2: Find a conditional branch whose EFLAGS dependency is tainted.
      for (auto &MBB : MF) {
        if (MBB.succ_size() <= 1) {
          continue;
        }

        MachineInstr *tainted_eflags_MI = nullptr;
        MachineInstr *conditional_branch_MI = nullptr;
        bool eflags_tainted = false;

        for (auto &MI : MBB) {
          if (isModifiesEFLAGS(MI)) {
            eflags_tainted = false;
            tainted_eflags_MI = nullptr;

            for (auto &MO : MI.explicit_operands()) {
              if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
                eflags_tainted = true;
                tainted_eflags_MI = &MI;
                break;
              }
            }
          }

          if (MI.isConditionalBranch() && isUsesEFLAGS(MI) && eflags_tainted) {
            conditional_branch_MI = &MI;
            break;
          }
        }

        if (conditional_branch_MI == nullptr || tainted_eflags_MI == nullptr) {
          continue;
        }

        SmallVector<MachineInstr *, 4> path_load_MIs;
        SmallSet<unsigned, 4> seen_load_bbs;

        for (MachineBasicBlock *Succ : MBB.successors()) {
          MachineInstr *load_MI = findFirstLoadOnBranchPath(Succ, &MBB, MPDT);
          if (load_MI && !seen_load_bbs.count(load_MI->getParent()->getNumber())) {
            path_load_MIs.push_back(load_MI);
            seen_load_bbs.insert(load_MI->getParent()->getNumber());
          }
        }

        if (path_load_MIs.empty()) {
          continue;
        }

        std::string branch_filename = "unknown";
        unsigned branch_line = 0;
        if (conditional_branch_MI->getDebugLoc()) {
          if (DILocation *Loc = conditional_branch_MI->getDebugLoc().get()) {
            branch_filename = Loc->getFilename().str();
            branch_line = Loc->getLine();
          }
        }

        std::string eflags_filename = "unknown";
        unsigned eflags_line = 0;
        if (tainted_eflags_MI->getDebugLoc()) {
          if (DILocation *Loc = tainted_eflags_MI->getDebugLoc().get()) {
            eflags_filename = Loc->getFilename().str();
            eflags_line = Loc->getLine();
          }
        }

        if (!markIfNew(SeenGadgets, getDedupKey("control-flow-load", conditional_branch_MI,
                                                path_load_MIs.front()))) {
          continue;
        }

        std::cout << " Control-flow load Gadgets Found in ---->  "
            << MF.getName().str()
            << "\n\t[+] Tainted EFLAGS at: " << eflags_filename << ":" << eflags_line
            << " (BB#" << tainted_eflags_MI->getParent()->getNumber() << ")"
            << "\n\t[+] Secret-dependent branch at: " << branch_filename << ":" << branch_line
            << " (BB#" << conditional_branch_MI->getParent()->getNumber() << ")"
            << "\n\t[+] Branch path load count: " << path_load_MIs.size()
            << "\n\t[+] Taint Info: livein " << livein_count
            << std::endl;

        for (MachineInstr *load_MI : path_load_MIs) {
          std::string load_filename = "unknown";
          unsigned load_line = 0;
          if (load_MI->getDebugLoc()) {
            if (DILocation *Loc = load_MI->getDebugLoc().get()) {
              load_filename = Loc->getFilename().str();
              load_line = Loc->getLine();
            }
          }

          std::cout << "\t    load at: " << load_filename << ":" << load_line
              << " (BB#" << load_MI->getParent()->getNumber() << ")"
              << std::endl;
        }

        goto end_search3;
      }

      end_search3:
        taint_register_list.clear();
        ++livein_count;
    }
  }


  // ###########################################################################
  //
  // Gadget searching #4:
  //         for data-flow load gadgets
  //
  // Pattern:
  //         secret -> taint_register_list
  //         tainted register -> load address computation
  //         load uses tainted register as an explicit operand
  //
  // This matches the vulnerable pattern:
  //
  //         idx = f(secret);
  //         value = array[idx];
  //
  // ###########################################################################
  { // Gadget searching #4 scope

    SmallDenseMap<unsigned, unsigned, 32> taint_register_list;

    int livein_count = 0;

    for (auto &Li : MF.begin()->liveins()) {

      taint_register_list[Li.PhysReg] = Li.PhysReg;

      // Step1: Track all registers explicitly propagated from the livein.
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {
          for (auto &MO : MI.explicit_operands()) {

            if (MO.isReg() && MO.isUse() && (MO.getReg() == Li.PhysReg)) {
              for (auto &MO_tmp : MI.defs()) {
                if (MO_tmp.isReg()) {
                  taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
                }
              }
            }

            else if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
              for (auto &MO_tmp : MI.defs()) {
                if (MO_tmp.isReg()) {
                  taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
                }
              }
            }
          }
        }
      }

      // Step2: Find a load whose explicit operands use a tainted register.
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {
          if (!MI.mayLoad() || MI.isCall()) {
            continue;
          }

          unsigned tainted_reg = 0;
          for (auto &MO : MI.explicit_operands()) {
            if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
              tainted_reg = MO.getReg();
              break;
            }
          }

          if (!tainted_reg) {
            continue;
          }

          std::string load_filename = "unknown";
          unsigned load_line = 0;
          if (MI.getDebugLoc()) {
            if (DILocation *Loc = MI.getDebugLoc().get()) {
              load_filename = Loc->getFilename().str();
              load_line = Loc->getLine();
            }
          }

          if (!markIfNew(SeenGadgets, getDedupKey("data-flow-load", &MI))) {
            continue;
          }

          std::cout << " Data-flow load Gadgets Found in ---->  "
              << MF.getName().str()
              << "\n\t[+] Tainted load at: " << load_filename << ":" << load_line
              << " (BB#" << MI.getParent()->getNumber() << ")"
              << "\n\t[+] Taint Info: livein " << livein_count
              << ", tainted register " << tainted_reg
              << std::endl;

          goto end_search4;
        }
      }

      end_search4:
        taint_register_list.clear();
        ++livein_count;
    }
  }


  // ###########################################################################
  //
  // Gadget searching #5:
  //         for control-flow store gadgets
  //
  // Pattern:
  //         secret -> taint_register_list
  //         tainted register -> OP_define_eflags (e.g. cmp/test/add/sub/...)
  //         OP_define_eflags -> conditional branch (Jcc)
  //         taken/fallthrough branch region contains store instruction(s)
  //
  // This matches the vulnerable pattern:
  //
  //         if (secret-dependent condition) {
  //             store(...);
  //         } else {
  //             store(...);
  //         }
  //
  // ###########################################################################
  { // Gadget searching #5 scope

    SmallDenseMap<unsigned, unsigned, 32> taint_register_list;

    int livein_count = 0;

    for (auto &Li : MF.begin()->liveins()) {

      taint_register_list[Li.PhysReg] = Li.PhysReg;

      // Step1: Track all registers explicitly propagated from the livein.
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {
          for (auto &MO : MI.explicit_operands()) {

            if (MO.isReg() && MO.isUse() && (MO.getReg() == Li.PhysReg)) {
              for (auto &MO_tmp : MI.defs()) {
                if (MO_tmp.isReg()) {
                  taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
                }
              }
            }

            else if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
              for (auto &MO_tmp : MI.defs()) {
                if (MO_tmp.isReg()) {
                  taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
                }
              }
            }
          }
        }
      }

      // Step2: Find a conditional branch whose EFLAGS dependency is tainted.
      for (auto &MBB : MF) {
        if (MBB.succ_size() <= 1) {
          continue;
        }

        MachineInstr *tainted_eflags_MI = nullptr;
        MachineInstr *conditional_branch_MI = nullptr;
        bool eflags_tainted = false;

        for (auto &MI : MBB) {
          if (isModifiesEFLAGS(MI)) {
            eflags_tainted = false;
            tainted_eflags_MI = nullptr;

            for (auto &MO : MI.explicit_operands()) {
              if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
                eflags_tainted = true;
                tainted_eflags_MI = &MI;
                break;
              }
            }
          }

          if (MI.isConditionalBranch() && isUsesEFLAGS(MI) && eflags_tainted) {
            conditional_branch_MI = &MI;
            break;
          }
        }

        if (conditional_branch_MI == nullptr || tainted_eflags_MI == nullptr) {
          continue;
        }

        SmallVector<MachineInstr *, 4> path_store_MIs;
        SmallSet<unsigned, 4> seen_store_bbs;

        for (MachineBasicBlock *Succ : MBB.successors()) {
          MachineInstr *store_MI = findFirstStoreOnBranchPath(Succ, &MBB, MPDT);
          if (store_MI && !seen_store_bbs.count(store_MI->getParent()->getNumber())) {
            path_store_MIs.push_back(store_MI);
            seen_store_bbs.insert(store_MI->getParent()->getNumber());
          }
        }

        if (path_store_MIs.empty()) {
          continue;
        }

        std::string branch_filename = "unknown";
        unsigned branch_line = 0;
        if (conditional_branch_MI->getDebugLoc()) {
          if (DILocation *Loc = conditional_branch_MI->getDebugLoc().get()) {
            branch_filename = Loc->getFilename().str();
            branch_line = Loc->getLine();
          }
        }

        std::string eflags_filename = "unknown";
        unsigned eflags_line = 0;
        if (tainted_eflags_MI->getDebugLoc()) {
          if (DILocation *Loc = tainted_eflags_MI->getDebugLoc().get()) {
            eflags_filename = Loc->getFilename().str();
            eflags_line = Loc->getLine();
          }
        }

        if (!markIfNew(SeenGadgets, getDedupKey("control-flow-store", conditional_branch_MI,
                                                path_store_MIs.front()))) {
          continue;
        }

        std::cout << " Control-flow store Gadgets Found in ---->  "
            << MF.getName().str()
            << "\n\t[+] Tainted EFLAGS at: " << eflags_filename << ":" << eflags_line
            << " (BB#" << tainted_eflags_MI->getParent()->getNumber() << ")"
            << "\n\t[+] Secret-dependent branch at: " << branch_filename << ":" << branch_line
            << " (BB#" << conditional_branch_MI->getParent()->getNumber() << ")"
            << "\n\t[+] Branch path store count: " << path_store_MIs.size()
            << "\n\t[+] Taint Info: livein " << livein_count
            << std::endl;

        for (MachineInstr *store_MI : path_store_MIs) {
          std::string store_filename = "unknown";
          unsigned store_line = 0;
          if (store_MI->getDebugLoc()) {
            if (DILocation *Loc = store_MI->getDebugLoc().get()) {
              store_filename = Loc->getFilename().str();
              store_line = Loc->getLine();
            }
          }

          std::cout << "\t    store at: " << store_filename << ":" << store_line
              << " (BB#" << store_MI->getParent()->getNumber() << ")"
              << std::endl;
        }

        goto end_search5;
      }

      end_search5:
        taint_register_list.clear();
        ++livein_count;
    }
  }


  // ###########################################################################
  //
  // Gadget searching #6:
  //         for data-flow store gadgets
  //
  // Pattern:
  //         secret -> taint_register_list
  //         tainted register -> store address/value computation
  //         store uses tainted register as an explicit operand
  //
  // This matches the vulnerable patterns:
  //
  //         idx = f(secret);
  //         array[idx] = value;
  //
  //         value = f(secret);
  //         *ptr = value;
  //
  // ###########################################################################
  { // Gadget searching #6 scope

    SmallDenseMap<unsigned, unsigned, 32> taint_register_list;

    int livein_count = 0;

    for (auto &Li : MF.begin()->liveins()) {

      taint_register_list[Li.PhysReg] = Li.PhysReg;

      // Step1: Track all registers explicitly propagated from the livein.
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {
          for (auto &MO : MI.explicit_operands()) {

            if (MO.isReg() && MO.isUse() && (MO.getReg() == Li.PhysReg)) {
              for (auto &MO_tmp : MI.defs()) {
                if (MO_tmp.isReg()) {
                  taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
                }
              }
            }

            else if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
              for (auto &MO_tmp : MI.defs()) {
                if (MO_tmp.isReg()) {
                  taint_register_list[MO_tmp.getReg()] = MO_tmp.getReg();
                }
              }
            }
          }
        }
      }

      // Step2: Find a store whose explicit operands use a tainted register.
      for (auto &MBB : MF) {
        for (auto &MI : MBB) {
          if (!MI.mayStore() || MI.isCall()) {
            continue;
          }

          unsigned tainted_reg = 0;
          for (auto &MO : MI.explicit_operands()) {
            if (MO.isReg() && MO.isUse() && taint_register_list.count(MO.getReg())) {
              tainted_reg = MO.getReg();
              break;
            }
          }

          if (!tainted_reg) {
            continue;
          }

          std::string store_filename = "unknown";
          unsigned store_line = 0;
          if (MI.getDebugLoc()) {
            if (DILocation *Loc = MI.getDebugLoc().get()) {
              store_filename = Loc->getFilename().str();
              store_line = Loc->getLine();
            }
          }

          if (!markIfNew(SeenGadgets, getDedupKey("data-flow-store", &MI))) {
            continue;
          }

          std::cout << " Data-flow store Gadgets Found in ---->  "
              << MF.getName().str()
              << "\n\t[+] Tainted store at: " << store_filename << ":" << store_line
              << " (BB#" << MI.getParent()->getNumber() << ")"
              << "\n\t[+] Taint Info: livein " << livein_count
              << ", tainted register " << tainted_reg
              << std::endl;

          goto end_search6;
        }
      }

      end_search6:
        taint_register_list.clear();
        ++livein_count;
    }
  }

  return true;
}

INITIALIZE_PASS_BEGIN(X86MIRAnalyzePass, PASS_KEY,
                      "X86 MachineIR Analyser", false, false)
INITIALIZE_PASS_END(X86MIRAnalyzePass, PASS_KEY,
                    "X86 MachineIR Analyser", false, false)

FunctionPass *llvm::createX86MIRAnalyzePass() {
  return new X86MIRAnalyzePass();
}
