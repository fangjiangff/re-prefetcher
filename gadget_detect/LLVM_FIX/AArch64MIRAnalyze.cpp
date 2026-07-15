#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"

#include <iostream>
#include <iterator>
#include <sstream>
#include <memory>
#include <string>

using namespace llvm;

#define PASS_KEY "aarch64-miranalyze"

static cl::opt<bool> EnableAArch64MIRAnalyze(
    "aarch64-mir-analyze", cl::desc("start AArch64 MIR analyzing"),
    cl::init(false), cl::Hidden);

static bool containsNameToken(StringRef Name, StringRef Token) {
  size_t Pos = 0;
  while ((Pos = Name.find(Token, Pos)) != StringRef::npos) {
    size_t End = Pos + Token.size();
    bool HasLeftBoundary = Pos == 0 || Name[Pos - 1] == '_';
    bool HasRightBoundary = End == Name.size() || Name[End] == '_';
    if (HasLeftBoundary && HasRightBoundary)
      return true;
    Pos = End;
  }

  return false;
}

static bool isSecretName(StringRef Name) {
  std::string LowerNameStorage = Name.lower();
  StringRef LowerName(LowerNameStorage);

  static const StringRef SecretFragments[] = {
      "secret",        "key",           "private",      "password",
      "passwd",        "passphrase",    "premaster",    "master_secret",
      "shared_secret", "sharedsecret",  "exponent"};
  for (StringRef Fragment : SecretFragments)
    if (LowerName.contains(Fragment))
      return true;

  static const StringRef SecretTokens[] = {
      "sk", "priv", "pwd", "psk", "ikm", "ctx", "exp"};
  for (StringRef Token : SecretTokens)
    if (containsNameToken(LowerName, Token))
      return true;

  return false;
}

static bool isSecretArgumentIndex(const Function &F, unsigned ArgIndex) {
  if (ArgIndex >= F.arg_size())
    return false;

  auto ArgIt = F.arg_begin();
  std::advance(ArgIt, ArgIndex);
  if (isSecretName(ArgIt->getName()))
    return true;

  const DISubprogram *SP = F.getSubprogram();
  if (!SP)
    return false;

  for (const Metadata *Node : SP->getRetainedNodes()) {
    const auto *Var = dyn_cast<DILocalVariable>(Node);
    if (Var && Var->isParameter() && Var->getArg() == ArgIndex + 1 &&
        isSecretName(Var->getName()))
      return true;
  }

  return false;
}

static int getAArch64GPRArgumentIndex(Register Reg,
                                      const TargetRegisterInfo *TRI) {
  static const MCPhysReg GPRArgs[] = {
      AArch64::X0, AArch64::X1, AArch64::X2, AArch64::X3,
      AArch64::X4, AArch64::X5, AArch64::X6, AArch64::X7};

  if (!Reg.isPhysical() || !TRI)
    return -1;

  for (unsigned I = 0; I < sizeof(GPRArgs) / sizeof(GPRArgs[0]); ++I)
    if (TRI->regsOverlap(Reg, GPRArgs[I]))
      return I;

  return -1;
}

static bool getSecretArgumentIndexForLiveIn(
    const MachineFunction &MF, Register LiveIn,
    const TargetRegisterInfo *TRI, unsigned &ArgIndex) {
  int Index = getAArch64GPRArgumentIndex(LiveIn, TRI);
  const Function &F = MF.getFunction();
  if (Index < 0 || static_cast<unsigned>(Index) >= F.arg_size())
    return false;

  if (!isSecretArgumentIndex(F, Index))
    return false;

  ArgIndex = Index;
  return true;
}

namespace {
class AArch64MIRAnalyzePass : public MachineFunctionPass {
public:
  AArch64MIRAnalyzePass() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "AArch64 MIR Analyser"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  static char ID;

private:
  const AArch64Subtarget *Subtarget = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  const AArch64InstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;

  MachineDominatorTree *MDT = nullptr;
  MachinePostDominatorTree *MPDT = nullptr;
  std::unique_ptr<MachineDominatorTree> OwnedMDT;
  std::unique_ptr<MachinePostDominatorTree> OwnedMPDT;
};
}

static bool isRegTainted(Register Reg,
                         const SmallDenseMap<unsigned, unsigned, 32> &TaintRegs,
                         const TargetRegisterInfo *TRI) {
  if (!Reg.isValid())
    return false;

  if (TaintRegs.count(Reg.id()))
    return true;

  if (!TRI || !Reg.isPhysical())
    return false;

  for (const auto &Entry : TaintRegs) {
    Register TaintedReg = Register(Entry.first);
    if (TaintedReg.isPhysical() && TRI->regsOverlap(Reg, TaintedReg))
      return true;
  }

  return false;
}

static void markRegTainted(Register Reg,
                           SmallDenseMap<unsigned, unsigned, 32> &TaintRegs) {
  if (Reg.isValid())
    TaintRegs[Reg.id()] = Reg.id();
}

static bool usesTaintedExplicitReg(
    const MachineInstr &MI, const SmallDenseMap<unsigned, unsigned, 32> &TaintRegs,
    const TargetRegisterInfo *TRI, Register *TaintedReg = nullptr) {
  for (const MachineOperand &MO : MI.explicit_operands()) {
    if (!MO.isReg() || !MO.isUse())
      continue;

    Register Reg = MO.getReg();
    if (isRegTainted(Reg, TaintRegs, TRI)) {
      if (TaintedReg)
        *TaintedReg = Reg;
      return true;
    }
  }

  return false;
}

static void propagateExplicitTaint(
    const MachineInstr &MI, SmallDenseMap<unsigned, unsigned, 32> &TaintRegs,
    const TargetRegisterInfo *TRI) {
  if (!usesTaintedExplicitReg(MI, TaintRegs, TRI))
    return;

  for (const MachineOperand &MO : MI.defs()) {
    if (MO.isReg())
      markRegTainted(MO.getReg(), TaintRegs);
  }
}

static bool modifiesNZCV(const MachineInstr &MI, const TargetRegisterInfo *TRI) {
  if (TRI && MI.modifiesRegister(AArch64::NZCV, TRI))
    return true;

  for (const MachineOperand &MO : MI.implicit_operands()) {
    if (MO.isReg() && MO.isDef() && MO.getReg() == AArch64::NZCV)
      return true;
  }

  return false;
}

static bool usesNZCV(const MachineInstr &MI, const TargetRegisterInfo *TRI) {
  if (TRI && MI.readsRegister(AArch64::NZCV, TRI))
    return true;

  for (const MachineOperand &MO : MI.implicit_operands()) {
    if (MO.isReg() && MO.isUse() && MO.getReg() == AArch64::NZCV)
      return true;
  }

  return false;
}

static std::string filenameFromMI(const MachineInstr *MI) {
  if (!MI || !MI->getDebugLoc())
    return "unknown";
  if (DILocation *Loc = MI->getDebugLoc().get())
    return Loc->getFilename().str();
  return "unknown";
}

static unsigned lineFromMI(const MachineInstr *MI) {
  if (!MI || !MI->getDebugLoc())
    return 0;
  if (DILocation *Loc = MI->getDebugLoc().get())
    return Loc->getLine();
  return 0;
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

enum class MemoryGadgetKind { Load, Store, SoftwarePrefetch };

static bool isSoftwarePrefetch(const MachineInstr &MI,
                               const AArch64InstrInfo *TII) {
  if (!TII)
    return false;

  StringRef OpcodeName = TII->getName(MI.getOpcode());
  return OpcodeName.starts_with("PRFM") || OpcodeName.starts_with("PRFUM");
}

static bool matchesMemoryGadgetKind(const MachineInstr &MI,
                                    MemoryGadgetKind Kind,
                                    const AArch64InstrInfo *TII) {
  if (MI.isCall())
    return false;

  bool IsSWP = isSoftwarePrefetch(MI, TII);
  switch (Kind) {
  case MemoryGadgetKind::Load:
    return MI.mayLoad() && !IsSWP;
  case MemoryGadgetKind::Store:
    return MI.mayStore() && !IsSWP;
  case MemoryGadgetKind::SoftwarePrefetch:
    return IsSWP;
  }

  return false;
}

static const char *controlFlowMarker(MemoryGadgetKind Kind) {
  switch (Kind) {
  case MemoryGadgetKind::Load:
    return " Control-flow load Gadgets Found in ---->  ";
  case MemoryGadgetKind::Store:
    return " Control-flow store Gadgets Found in ---->  ";
  case MemoryGadgetKind::SoftwarePrefetch:
    return " Control-flow swp Gadgets Found in ---->  ";
  }

  return " Control-flow unknown Gadgets Found in ---->  ";
}

static const char *dataFlowMarker(MemoryGadgetKind Kind) {
  switch (Kind) {
  case MemoryGadgetKind::Load:
    return " Data-flow load Gadgets Found in ---->  ";
  case MemoryGadgetKind::Store:
    return " Data-flow store Gadgets Found in ---->  ";
  case MemoryGadgetKind::SoftwarePrefetch:
    return " Data-flow swp Gadgets Found in ---->  ";
  }

  return " Data-flow unknown Gadgets Found in ---->  ";
}

static const char *memoryKindName(MemoryGadgetKind Kind) {
  switch (Kind) {
  case MemoryGadgetKind::Load:
    return "load";
  case MemoryGadgetKind::Store:
    return "store";
  case MemoryGadgetKind::SoftwarePrefetch:
    return "swp";
  }

  return "unknown";
}

static StringRef controlFlowType(MemoryGadgetKind Kind) {
  switch (Kind) {
  case MemoryGadgetKind::Load:
    return "control-flow-load";
  case MemoryGadgetKind::Store:
    return "control-flow-store";
  case MemoryGadgetKind::SoftwarePrefetch:
    return "control-flow-swp";
  }

  return "control-flow-unknown";
}

static StringRef dataFlowType(MemoryGadgetKind Kind) {
  switch (Kind) {
  case MemoryGadgetKind::Load:
    return "data-flow-load";
  case MemoryGadgetKind::Store:
    return "data-flow-store";
  case MemoryGadgetKind::SoftwarePrefetch:
    return "data-flow-swp";
  }

  return "data-flow-unknown";
}

static MachineInstr *findFirstMemoryOpOnBranchPath(MachineBasicBlock *Start,
                                                   MachineBasicBlock *BranchMBB,
                                                   MachinePostDominatorTree *MPDT,
                                                   MemoryGadgetKind Kind,
                                                   const AArch64InstrInfo *TII) {
  SmallVector<MachineBasicBlock *, 8> WorkList;
  SmallPtrSet<MachineBasicBlock *, 16> Visited;

  WorkList.push_back(Start);

  while (!WorkList.empty()) {
    MachineBasicBlock *MBB = WorkList.pop_back_val();
    if (!Visited.insert(MBB).second)
      continue;

    if (MPDT && MPDT->dominates(MBB, BranchMBB))
      continue;

    for (MachineInstr &MI : *MBB) {
      if (matchesMemoryGadgetKind(MI, Kind, TII))
        return &MI;
    }

    for (MachineBasicBlock *Succ : MBB->successors()) {
      if (!Visited.count(Succ))
        WorkList.push_back(Succ);
    }
  }

  return nullptr;
}

static void seedAndPropagateLiveInTaint(
    MachineFunction &MF, Register LiveIn,
    SmallDenseMap<unsigned, unsigned, 32> &TaintRegs,
    const TargetRegisterInfo *TRI) {
  markRegTainted(LiveIn, TaintRegs);

  for (auto &MBB : MF) {
    for (auto &MI : MBB)
      propagateExplicitTaint(MI, TaintRegs, TRI);
  }
}

static bool isSecretDependentBranch(
    MachineBasicBlock &MBB, const SmallDenseMap<unsigned, unsigned, 32> &TaintRegs,
    const TargetRegisterInfo *TRI, MachineInstr *&TaintedCondMI,
    MachineInstr *&BranchMI) {
  bool NZCVTainted = false;
  MachineInstr *NZCVDefMI = nullptr;

  for (MachineInstr &MI : MBB) {
    if (modifiesNZCV(MI, TRI)) {
      NZCVTainted = usesTaintedExplicitReg(MI, TaintRegs, TRI);
      NZCVDefMI = NZCVTainted ? &MI : nullptr;
    }

    if (!MI.isConditionalBranch())
      continue;

    Register TaintedReg;
    if (usesTaintedExplicitReg(MI, TaintRegs, TRI, &TaintedReg)) {
      TaintedCondMI = &MI;
      BranchMI = &MI;
      return true;
    }

    if (usesNZCV(MI, TRI) && NZCVTainted) {
      TaintedCondMI = NZCVDefMI;
      BranchMI = &MI;
      return true;
    }
  }

  return false;
}

static bool reportControlFlowMemoryGadget(
    MachineFunction &MF, MachinePostDominatorTree *MPDT,
    const SmallDenseMap<unsigned, unsigned, 32> &TaintRegs,
    const TargetRegisterInfo *TRI, const AArch64InstrInfo *TII, int ArgIndex,
    MemoryGadgetKind Kind, SmallSet<std::string, 32> &SeenGadgets) {
  for (auto &MBB : MF) {
    if (MBB.succ_size() <= 1)
      continue;

    MachineInstr *TaintedCondMI = nullptr;
    MachineInstr *BranchMI = nullptr;
    if (!isSecretDependentBranch(MBB, TaintRegs, TRI, TaintedCondMI, BranchMI))
      continue;

    SmallVector<MachineInstr *, 4> PathMemMIs;
    SmallSet<unsigned, 4> SeenMemBBs;

    for (MachineBasicBlock *Succ : MBB.successors()) {
      MachineInstr *MemMI =
          findFirstMemoryOpOnBranchPath(Succ, &MBB, MPDT, Kind, TII);
      if (MemMI && !SeenMemBBs.count(MemMI->getParent()->getNumber())) {
        PathMemMIs.push_back(MemMI);
        SeenMemBBs.insert(MemMI->getParent()->getNumber());
      }
    }

    if (PathMemMIs.empty())
      continue;

    const char *Marker = controlFlowMarker(Kind);
    const char *KindName = memoryKindName(Kind);
    StringRef Type = controlFlowType(Kind);

    if (!markIfNew(SeenGadgets, getDedupKey(Type, BranchMI, PathMemMIs.front())))
      continue;

    std::cout << Marker << MF.getName().str()
              << "\n\t[+] Tainted branch condition at: "
              << filenameFromMI(TaintedCondMI) << ":" << lineFromMI(TaintedCondMI)
              << " (BB#" << TaintedCondMI->getParent()->getNumber() << ")"
              << "\n\t[+] Secret-dependent branch at: "
              << filenameFromMI(BranchMI) << ":" << lineFromMI(BranchMI)
              << " (BB#" << BranchMI->getParent()->getNumber() << ")"
              << "\n\t[+] Branch path " << KindName << " count: " << PathMemMIs.size()
              << "\n\t[+] Taint Info: livein " << ArgIndex << std::endl;

    for (MachineInstr *MemMI : PathMemMIs) {
      std::cout << "\t    " << KindName << " at: " << filenameFromMI(MemMI) << ":"
                << lineFromMI(MemMI) << " (BB#"
                << MemMI->getParent()->getNumber() << ")" << std::endl;
    }

    return true;
  }

  return false;
}

static bool reportDataFlowMemoryGadget(
    MachineFunction &MF, const SmallDenseMap<unsigned, unsigned, 32> &TaintRegs,
    const TargetRegisterInfo *TRI, const AArch64InstrInfo *TII, int ArgIndex,
    MemoryGadgetKind Kind, SmallSet<std::string, 32> &SeenGadgets) {
  for (auto &MBB : MF) {
    for (auto &MI : MBB) {
      if (!matchesMemoryGadgetKind(MI, Kind, TII))
        continue;

      Register TaintedReg;
      if (!usesTaintedExplicitReg(MI, TaintRegs, TRI, &TaintedReg))
        continue;

      const char *Marker = dataFlowMarker(Kind);
      const char *KindName = memoryKindName(Kind);
      StringRef Type = dataFlowType(Kind);

      if (!markIfNew(SeenGadgets, getDedupKey(Type, &MI)))
        continue;

      std::cout << Marker << MF.getName().str()
                << "\n\t[+] Tainted " << KindName << " at: " << filenameFromMI(&MI)
                << ":" << lineFromMI(&MI) << " (BB#"
                << MI.getParent()->getNumber() << ")"
                << "\n\t[+] Taint Info: livein " << ArgIndex
                << ", tainted register " << TaintedReg.id() << std::endl;

      return true;
    }
  }

  return false;
}

char AArch64MIRAnalyzePass::ID = 0;

void AArch64MIRAnalyzePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool AArch64MIRAnalyzePass::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableAArch64MIRAnalyze)
    return false;

  Subtarget = &MF.getSubtarget<AArch64Subtarget>();
  MRI = &MF.getRegInfo();
  TII = Subtarget->getInstrInfo();
  TRI = Subtarget->getRegisterInfo();

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

  if (!MDT || !MPDT || MF.begin() == MF.end())
    return false;

  SmallSet<std::string, 32> SeenGadgets;

  for (auto &Li : MF.begin()->liveins()) {
    unsigned ArgIndex = 0;
    if (!getSecretArgumentIndexForLiveIn(MF, Li.PhysReg, TRI, ArgIndex))
      continue;
    SmallDenseMap<unsigned, unsigned, 32> TaintRegs;
    seedAndPropagateLiveInTaint(MF, Li.PhysReg, TaintRegs, TRI);

    reportControlFlowMemoryGadget(MF, MPDT, TaintRegs, TRI, TII, ArgIndex,
                                  MemoryGadgetKind::Load, SeenGadgets);
    reportDataFlowMemoryGadget(MF, TaintRegs, TRI, TII, ArgIndex,
                               MemoryGadgetKind::Load, SeenGadgets);
    reportControlFlowMemoryGadget(MF, MPDT, TaintRegs, TRI, TII, ArgIndex,
                                  MemoryGadgetKind::Store, SeenGadgets);
    reportDataFlowMemoryGadget(MF, TaintRegs, TRI, TII, ArgIndex,
                               MemoryGadgetKind::Store, SeenGadgets);
    reportControlFlowMemoryGadget(MF, MPDT, TaintRegs, TRI, TII, ArgIndex,
                                  MemoryGadgetKind::SoftwarePrefetch,
                                  SeenGadgets);
    reportDataFlowMemoryGadget(MF, TaintRegs, TRI, TII, ArgIndex,
                               MemoryGadgetKind::SoftwarePrefetch,
                               SeenGadgets);

  }

  return true;
}

INITIALIZE_PASS_BEGIN(AArch64MIRAnalyzePass, PASS_KEY,
                      "AArch64 MachineIR Analyser", false, false)
INITIALIZE_PASS_END(AArch64MIRAnalyzePass, PASS_KEY,
                    "AArch64 MachineIR Analyser", false, false)

FunctionPass *llvm::createAArch64MIRAnalyzePass() {
  return new AArch64MIRAnalyzePass();
}
