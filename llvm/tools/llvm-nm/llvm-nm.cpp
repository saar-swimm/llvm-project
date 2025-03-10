//===-- llvm-nm.cpp - Symbol table dumping utility for llvm ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This program is a utility that works like traditional Unix "nm", that is, it
// prints out the names of symbols in a bitcode or object file, along with some
// information about each symbol.
//
// This "nm" supports many of the features of GNU "nm", including its different
// output formats.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/IRObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/TapiFile.h"
#include "llvm/Object/TapiUniversal.h"
#include "llvm/Object/Wasm.h"
#include "llvm/Object/XCOFFObjectFile.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

using namespace llvm;
using namespace object;

namespace {
using namespace llvm::opt; // for HelpHidden in Opts.inc
enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  OPT_##ID,
#include "Opts.inc"
#undef OPTION
};

#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "Opts.inc"
#undef PREFIX

const opt::OptTable::Info InfoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  {                                                                            \
      PREFIX,      NAME,      HELPTEXT,                                        \
      METAVAR,     OPT_##ID,  opt::Option::KIND##Class,                        \
      PARAM,       FLAGS,     OPT_##GROUP,                                     \
      OPT_##ALIAS, ALIASARGS, VALUES},
#include "Opts.inc"
#undef OPTION
};

class NmOptTable : public opt::OptTable {
public:
  NmOptTable() : OptTable(InfoTable) { setGroupedShortOptions(true); }
};

enum OutputFormatTy { bsd, sysv, posix, darwin, just_symbols };
} // namespace

static bool ArchiveMap;
static bool DebugSyms;
static bool DefinedOnly;
static bool Demangle;
static bool DynamicSyms;
static bool ExternalOnly;
static OutputFormatTy OutputFormat;
static bool NoLLVMBitcode;
static bool NoSort;
static bool NoWeakSymbols;
static bool NumericSort;
static bool PrintFileName;
static bool PrintSize;
static bool Quiet;
static bool ReverseSort;
static bool SpecialSyms;
static bool SizeSort;
static bool UndefinedOnly;
static bool WithoutAliases;

namespace {
enum Radix { d, o, x };
} // namespace
static Radix AddressRadix;

// Mach-O specific options.
static bool ArchAll = false;
static std::vector<StringRef> ArchFlags;
static bool AddDyldInfo;
static bool AddInlinedInfo;
static bool DyldInfoOnly;
static bool FormatMachOasHex;
static bool NoDyldInfo;
static std::vector<StringRef> SegSect;
static bool MachOPrintSizeWarning = false;

// Miscellaneous states.
static bool PrintAddress = true;
static bool MultipleFiles = false;
static bool HadError = false;

static StringRef ToolName;

static void warn(Error Err, Twine FileName, Twine Context = Twine()) {
  assert(Err);

  // Flush the standard output so that the warning isn't interleaved with other
  // output if stdout and stderr are writing to the same place.
  outs().flush();

  handleAllErrors(std::move(Err), [&](const ErrorInfoBase &EI) {
    WithColor::warning(errs(), ToolName)
        << FileName << ": " << (Context.str().empty() ? "" : Context + ": ")
        << EI.message() << "\n";
  });
}

static void error(Twine Message, Twine Path = Twine()) {
  HadError = true;
  WithColor::error(errs(), ToolName) << Path << ": " << Message << "\n";
}

static bool error(std::error_code EC, Twine Path = Twine()) {
  if (EC) {
    error(EC.message(), Path);
    return true;
  }
  return false;
}

// This version of error() prints the archive name and member name, for example:
// "libx.a(foo.o)" after the ToolName before the error message.  It sets
// HadError but returns allowing the code to move on to other archive members.
static void error(llvm::Error E, StringRef FileName, const Archive::Child &C,
                  StringRef ArchitectureName = StringRef()) {
  HadError = true;
  WithColor::error(errs(), ToolName) << FileName;

  Expected<StringRef> NameOrErr = C.getName();
  // TODO: if we have a error getting the name then it would be nice to print
  // the index of which archive member this is and or its offset in the
  // archive instead of "???" as the name.
  if (!NameOrErr) {
    consumeError(NameOrErr.takeError());
    errs() << "(" << "???" << ")";
  } else
    errs() << "(" << NameOrErr.get() << ")";

  if (!ArchitectureName.empty())
    errs() << " (for architecture " << ArchitectureName << ")";

  std::string Buf;
  raw_string_ostream OS(Buf);
  logAllUnhandledErrors(std::move(E), OS);
  OS.flush();
  errs() << ": " << Buf << "\n";
}

// This version of error() prints the file name and which architecture slice it
// is from, for example: "foo.o (for architecture i386)" after the ToolName
// before the error message.  It sets HadError but returns allowing the code to
// move on to other architecture slices.
static void error(llvm::Error E, StringRef FileName,
                  StringRef ArchitectureName = StringRef()) {
  HadError = true;
  WithColor::error(errs(), ToolName) << FileName;

  if (!ArchitectureName.empty())
    errs() << " (for architecture " << ArchitectureName << ")";

  std::string Buf;
  raw_string_ostream OS(Buf);
  logAllUnhandledErrors(std::move(E), OS);
  OS.flush();
  errs() << ": " << Buf << "\n";
}

namespace {
struct NMSymbol {
  uint64_t Address;
  uint64_t Size;
  char TypeChar;
  std::string Name;
  StringRef SectionName;
  StringRef TypeName;
  BasicSymbolRef Sym;
  // The Sym field above points to the native symbol in the object file,
  // for Mach-O when we are creating symbols from the dyld info the above
  // pointer is null as there is no native symbol.  In these cases the fields
  // below are filled in to represent what would have been a Mach-O nlist
  // native symbol.
  uint32_t SymFlags;
  SectionRef Section;
  uint8_t NType;
  uint8_t NSect;
  uint16_t NDesc;
  std::string IndirectName;
};
} // anonymous namespace

static bool compareSymbolAddress(const NMSymbol &A, const NMSymbol &B) {
  bool ADefined;
  // Symbol flags have been checked in the caller.
  if (A.Sym.getRawDataRefImpl().p) {
    uint32_t AFlags = cantFail(A.Sym.getFlags());
    ADefined = !(AFlags & SymbolRef::SF_Undefined);
  } else {
    ADefined = A.TypeChar != 'U';
  }
  bool BDefined;
  // Symbol flags have been checked in the caller.
  if (B.Sym.getRawDataRefImpl().p) {
    uint32_t BFlags = cantFail(B.Sym.getFlags());
    BDefined = !(BFlags & SymbolRef::SF_Undefined);
  } else {
    BDefined = B.TypeChar != 'U';
  }
  return std::make_tuple(ADefined, A.Address, A.Name, A.Size) <
         std::make_tuple(BDefined, B.Address, B.Name, B.Size);
}

static bool compareSymbolSize(const NMSymbol &A, const NMSymbol &B) {
  return std::make_tuple(A.Size, A.Name, A.Address) <
         std::make_tuple(B.Size, B.Name, B.Address);
}

static bool compareSymbolName(const NMSymbol &A, const NMSymbol &B) {
  return std::make_tuple(A.Name, A.Size, A.Address) <
         std::make_tuple(B.Name, B.Size, B.Address);
}

static char isSymbolList64Bit(SymbolicFile &Obj) {
  if (auto *IRObj = dyn_cast<IRObjectFile>(&Obj))
    return Triple(IRObj->getTargetTriple()).isArch64Bit();
  if (isa<COFFObjectFile>(Obj) || isa<COFFImportFile>(Obj))
    return false;
  if (XCOFFObjectFile *XCOFFObj = dyn_cast<XCOFFObjectFile>(&Obj))
    return XCOFFObj->is64Bit();

  if (isa<WasmObjectFile>(Obj))
    return false;
  if (TapiFile *Tapi = dyn_cast<TapiFile>(&Obj))
    return Tapi->is64Bit();
  if (MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(&Obj))
    return MachO->is64Bit();
  return cast<ELFObjectFileBase>(Obj).getBytesInAddress() == 8;
}

static StringRef CurrentFilename;
static std::vector<NMSymbol> SymbolList;

static char getSymbolNMTypeChar(IRObjectFile &Obj, basic_symbol_iterator I);

// darwinPrintSymbol() is used to print a symbol from a Mach-O file when the
// the OutputFormat is darwin or we are printing Mach-O symbols in hex.  For
// the darwin format it produces the same output as darwin's nm(1) -m output
// and when printing Mach-O symbols in hex it produces the same output as
// darwin's nm(1) -x format.
static void darwinPrintSymbol(SymbolicFile &Obj, const NMSymbol &S,
                              char *SymbolAddrStr, const char *printBlanks,
                              const char *printDashes,
                              const char *printFormat) {
  MachO::mach_header H;
  MachO::mach_header_64 H_64;
  uint32_t Filetype = MachO::MH_OBJECT;
  uint32_t Flags = 0;
  uint8_t NType = 0;
  uint8_t NSect = 0;
  uint16_t NDesc = 0;
  uint32_t NStrx = 0;
  uint64_t NValue = 0;
  MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(&Obj);
  if (Obj.isIR()) {
    uint32_t SymFlags = cantFail(S.Sym.getFlags());
    if (SymFlags & SymbolRef::SF_Global)
      NType |= MachO::N_EXT;
    if (SymFlags & SymbolRef::SF_Hidden)
      NType |= MachO::N_PEXT;
    if (SymFlags & SymbolRef::SF_Undefined)
      NType |= MachO::N_EXT | MachO::N_UNDF;
    else {
      // Here we have a symbol definition.  So to fake out a section name we
      // use 1, 2 and 3 for section numbers.  See below where they are used to
      // print out fake section names.
      NType |= MachO::N_SECT;
      if (SymFlags & SymbolRef::SF_Const)
        NSect = 3;
      else if (SymFlags & SymbolRef::SF_Executable)
        NSect = 1;
      else
        NSect = 2;
    }
    if (SymFlags & SymbolRef::SF_Weak)
      NDesc |= MachO::N_WEAK_DEF;
  } else {
    DataRefImpl SymDRI = S.Sym.getRawDataRefImpl();
    if (MachO->is64Bit()) {
      H_64 = MachO->MachOObjectFile::getHeader64();
      Filetype = H_64.filetype;
      Flags = H_64.flags;
      if (SymDRI.p){
        MachO::nlist_64 STE_64 = MachO->getSymbol64TableEntry(SymDRI);
        NType = STE_64.n_type;
        NSect = STE_64.n_sect;
        NDesc = STE_64.n_desc;
        NStrx = STE_64.n_strx;
        NValue = STE_64.n_value;
      } else {
        NType = S.NType;
        NSect = S.NSect;
        NDesc = S.NDesc;
        NStrx = 0;
        NValue = S.Address;
      }
    } else {
      H = MachO->MachOObjectFile::getHeader();
      Filetype = H.filetype;
      Flags = H.flags;
      if (SymDRI.p){
        MachO::nlist STE = MachO->getSymbolTableEntry(SymDRI);
        NType = STE.n_type;
        NSect = STE.n_sect;
        NDesc = STE.n_desc;
        NStrx = STE.n_strx;
        NValue = STE.n_value;
      } else {
        NType = S.NType;
        NSect = S.NSect;
        NDesc = S.NDesc;
        NStrx = 0;
        NValue = S.Address;
      }
    }
  }

  // If we are printing Mach-O symbols in hex do that and return.
  if (FormatMachOasHex) {
    outs() << format(printFormat, NValue) << ' '
           << format("%02x %02x %04x %08x", NType, NSect, NDesc, NStrx) << ' '
           << S.Name;
    if ((NType & MachO::N_TYPE) == MachO::N_INDR) {
      outs() << " (indirect for ";
      outs() << format(printFormat, NValue) << ' ';
      StringRef IndirectName;
      if (S.Sym.getRawDataRefImpl().p) {
        if (MachO->getIndirectName(S.Sym.getRawDataRefImpl(), IndirectName))
          outs() << "?)";
        else
          outs() << IndirectName << ")";
      } else
        outs() << S.IndirectName << ")";
    }
    outs() << "\n";
    return;
  }

  if (PrintAddress) {
    if ((NType & MachO::N_TYPE) == MachO::N_INDR)
      strcpy(SymbolAddrStr, printBlanks);
    if (Obj.isIR() && (NType & MachO::N_TYPE) == MachO::N_TYPE)
      strcpy(SymbolAddrStr, printDashes);
    outs() << SymbolAddrStr << ' ';
  }

  switch (NType & MachO::N_TYPE) {
  case MachO::N_UNDF:
    if (NValue != 0) {
      outs() << "(common) ";
      if (MachO::GET_COMM_ALIGN(NDesc) != 0)
        outs() << "(alignment 2^" << (int)MachO::GET_COMM_ALIGN(NDesc) << ") ";
    } else {
      if ((NType & MachO::N_TYPE) == MachO::N_PBUD)
        outs() << "(prebound ";
      else
        outs() << "(";
      if ((NDesc & MachO::REFERENCE_TYPE) ==
          MachO::REFERENCE_FLAG_UNDEFINED_LAZY)
        outs() << "undefined [lazy bound]) ";
      else if ((NDesc & MachO::REFERENCE_TYPE) ==
               MachO::REFERENCE_FLAG_PRIVATE_UNDEFINED_LAZY)
        outs() << "undefined [private lazy bound]) ";
      else if ((NDesc & MachO::REFERENCE_TYPE) ==
               MachO::REFERENCE_FLAG_PRIVATE_UNDEFINED_NON_LAZY)
        outs() << "undefined [private]) ";
      else
        outs() << "undefined) ";
    }
    break;
  case MachO::N_ABS:
    outs() << "(absolute) ";
    break;
  case MachO::N_INDR:
    outs() << "(indirect) ";
    break;
  case MachO::N_SECT: {
    if (Obj.isIR()) {
      // For llvm bitcode files print out a fake section name using the values
      // use 1, 2 and 3 for section numbers as set above.
      if (NSect == 1)
        outs() << "(LTO,CODE) ";
      else if (NSect == 2)
        outs() << "(LTO,DATA) ";
      else if (NSect == 3)
        outs() << "(LTO,RODATA) ";
      else
        outs() << "(?,?) ";
      break;
    }
    section_iterator Sec = SectionRef();
    if (S.Sym.getRawDataRefImpl().p) {
      Expected<section_iterator> SecOrErr =
          MachO->getSymbolSection(S.Sym.getRawDataRefImpl());
      if (!SecOrErr) {
        consumeError(SecOrErr.takeError());
        outs() << "(?,?) ";
        break;
      }
      Sec = *SecOrErr;
      if (Sec == MachO->section_end()) {
        outs() << "(?,?) ";
        break;
      }
    } else {
      Sec = S.Section;
    }
    DataRefImpl Ref = Sec->getRawDataRefImpl();
    StringRef SectionName;
    if (Expected<StringRef> NameOrErr = MachO->getSectionName(Ref))
      SectionName = *NameOrErr;
    StringRef SegmentName = MachO->getSectionFinalSegmentName(Ref);
    outs() << "(" << SegmentName << "," << SectionName << ") ";
    break;
  }
  default:
    outs() << "(?) ";
    break;
  }

  if (NType & MachO::N_EXT) {
    if (NDesc & MachO::REFERENCED_DYNAMICALLY)
      outs() << "[referenced dynamically] ";
    if (NType & MachO::N_PEXT) {
      if ((NDesc & MachO::N_WEAK_DEF) == MachO::N_WEAK_DEF)
        outs() << "weak private external ";
      else
        outs() << "private external ";
    } else {
      if ((NDesc & MachO::N_WEAK_REF) == MachO::N_WEAK_REF ||
          (NDesc & MachO::N_WEAK_DEF) == MachO::N_WEAK_DEF) {
        if ((NDesc & (MachO::N_WEAK_REF | MachO::N_WEAK_DEF)) ==
            (MachO::N_WEAK_REF | MachO::N_WEAK_DEF))
          outs() << "weak external automatically hidden ";
        else
          outs() << "weak external ";
      } else
        outs() << "external ";
    }
  } else {
    if (NType & MachO::N_PEXT)
      outs() << "non-external (was a private external) ";
    else
      outs() << "non-external ";
  }

  if (Filetype == MachO::MH_OBJECT) {
    if (NDesc & MachO::N_NO_DEAD_STRIP)
      outs() << "[no dead strip] ";
    if ((NType & MachO::N_TYPE) != MachO::N_UNDF &&
        NDesc & MachO::N_SYMBOL_RESOLVER)
      outs() << "[symbol resolver] ";
    if ((NType & MachO::N_TYPE) != MachO::N_UNDF && NDesc & MachO::N_ALT_ENTRY)
      outs() << "[alt entry] ";
    if ((NType & MachO::N_TYPE) != MachO::N_UNDF && NDesc & MachO::N_COLD_FUNC)
      outs() << "[cold func] ";
  }

  if ((NDesc & MachO::N_ARM_THUMB_DEF) == MachO::N_ARM_THUMB_DEF)
    outs() << "[Thumb] ";

  if ((NType & MachO::N_TYPE) == MachO::N_INDR) {
    outs() << S.Name << " (for ";
    StringRef IndirectName;
    if (MachO) {
      if (S.Sym.getRawDataRefImpl().p) {
        if (MachO->getIndirectName(S.Sym.getRawDataRefImpl(), IndirectName))
          outs() << "?)";
        else
          outs() << IndirectName << ")";
      } else
        outs() << S.IndirectName << ")";
    } else
      outs() << "?)";
  } else
    outs() << S.Name;

  if ((Flags & MachO::MH_TWOLEVEL) == MachO::MH_TWOLEVEL &&
      (((NType & MachO::N_TYPE) == MachO::N_UNDF && NValue == 0) ||
       (NType & MachO::N_TYPE) == MachO::N_PBUD)) {
    uint32_t LibraryOrdinal = MachO::GET_LIBRARY_ORDINAL(NDesc);
    if (LibraryOrdinal != 0) {
      if (LibraryOrdinal == MachO::EXECUTABLE_ORDINAL)
        outs() << " (from executable)";
      else if (LibraryOrdinal == MachO::DYNAMIC_LOOKUP_ORDINAL)
        outs() << " (dynamically looked up)";
      else {
        StringRef LibraryName;
        if (!MachO ||
            MachO->getLibraryShortNameByIndex(LibraryOrdinal - 1, LibraryName))
          outs() << " (from bad library ordinal " << LibraryOrdinal << ")";
        else
          outs() << " (from " << LibraryName << ")";
      }
    }
  }

  outs() << "\n";
}

// Table that maps Darwin's Mach-O stab constants to strings to allow printing.
struct DarwinStabName {
  uint8_t NType;
  const char *Name;
};
const struct DarwinStabName DarwinStabNames[] = {
    {MachO::N_GSYM, "GSYM"},
    {MachO::N_FNAME, "FNAME"},
    {MachO::N_FUN, "FUN"},
    {MachO::N_STSYM, "STSYM"},
    {MachO::N_LCSYM, "LCSYM"},
    {MachO::N_BNSYM, "BNSYM"},
    {MachO::N_PC, "PC"},
    {MachO::N_AST, "AST"},
    {MachO::N_OPT, "OPT"},
    {MachO::N_RSYM, "RSYM"},
    {MachO::N_SLINE, "SLINE"},
    {MachO::N_ENSYM, "ENSYM"},
    {MachO::N_SSYM, "SSYM"},
    {MachO::N_SO, "SO"},
    {MachO::N_OSO, "OSO"},
    {MachO::N_LSYM, "LSYM"},
    {MachO::N_BINCL, "BINCL"},
    {MachO::N_SOL, "SOL"},
    {MachO::N_PARAMS, "PARAM"},
    {MachO::N_VERSION, "VERS"},
    {MachO::N_OLEVEL, "OLEV"},
    {MachO::N_PSYM, "PSYM"},
    {MachO::N_EINCL, "EINCL"},
    {MachO::N_ENTRY, "ENTRY"},
    {MachO::N_LBRAC, "LBRAC"},
    {MachO::N_EXCL, "EXCL"},
    {MachO::N_RBRAC, "RBRAC"},
    {MachO::N_BCOMM, "BCOMM"},
    {MachO::N_ECOMM, "ECOMM"},
    {MachO::N_ECOML, "ECOML"},
    {MachO::N_LENG, "LENG"},
};

static const char *getDarwinStabString(uint8_t NType) {
  for (auto I : makeArrayRef(DarwinStabNames))
    if (I.NType == NType)
      return I.Name;
  return nullptr;
}

// darwinPrintStab() prints the n_sect, n_desc along with a symbolic name of
// a stab n_type value in a Mach-O file.
static void darwinPrintStab(MachOObjectFile *MachO, const NMSymbol &S) {
  MachO::nlist_64 STE_64;
  MachO::nlist STE;
  uint8_t NType;
  uint8_t NSect;
  uint16_t NDesc;
  DataRefImpl SymDRI = S.Sym.getRawDataRefImpl();
  if (MachO->is64Bit()) {
    STE_64 = MachO->getSymbol64TableEntry(SymDRI);
    NType = STE_64.n_type;
    NSect = STE_64.n_sect;
    NDesc = STE_64.n_desc;
  } else {
    STE = MachO->getSymbolTableEntry(SymDRI);
    NType = STE.n_type;
    NSect = STE.n_sect;
    NDesc = STE.n_desc;
  }

  outs() << format(" %02x %04x ", NSect, NDesc);
  if (const char *stabString = getDarwinStabString(NType))
    outs() << format("%5.5s", stabString);
  else
    outs() << format("   %02x", NType);
}

static Optional<std::string> demangle(const std::string &Name,
                                      bool StripUnderscore) {
  const char *Mangled = Name.c_str();
  if (StripUnderscore && Mangled[0] == '_')
    Mangled = Mangled + 1;

  std::string Demangled;
  if (nonMicrosoftDemangle(Mangled, Demangled))
    return Demangled;
  return None;
}

static bool symbolIsDefined(const NMSymbol &Sym) {
  return Sym.TypeChar != 'U' && Sym.TypeChar != 'w' && Sym.TypeChar != 'v';
}

static void writeFileName(raw_ostream &S, StringRef ArchiveName,
                          StringRef ArchitectureName) {
  if (!ArchitectureName.empty())
    S << "(for architecture " << ArchitectureName << "):";
  if (OutputFormat == posix && !ArchiveName.empty())
    S << ArchiveName << "[" << CurrentFilename << "]: ";
  else {
    if (!ArchiveName.empty())
      S << ArchiveName << ":";
    S << CurrentFilename << ": ";
  }
}

static void sortAndPrintSymbolList(SymbolicFile &Obj, bool printName,
                                   StringRef ArchiveName,
                                   StringRef ArchitectureName) {
  if (!NoSort) {
    using Comparator = bool (*)(const NMSymbol &, const NMSymbol &);
    Comparator Cmp;
    if (NumericSort)
      Cmp = &compareSymbolAddress;
    else if (SizeSort)
      Cmp = &compareSymbolSize;
    else
      Cmp = &compareSymbolName;

    if (ReverseSort)
      llvm::sort(SymbolList, [=](const NMSymbol &A, const NMSymbol &B) -> bool {
        return Cmp(B, A);
      });
    else
      llvm::sort(SymbolList, Cmp);
  }

  if (!PrintFileName) {
    if ((OutputFormat == bsd || OutputFormat == posix ||
         OutputFormat == just_symbols) &&
        MultipleFiles && printName) {
      outs() << '\n' << CurrentFilename << ":\n";
    } else if (OutputFormat == sysv) {
      outs() << "\n\nSymbols from " << CurrentFilename << ":\n\n";
      if (isSymbolList64Bit(Obj))
        outs() << "Name                  Value           Class        Type"
               << "         Size             Line  Section\n";
      else
        outs() << "Name                  Value   Class        Type"
               << "         Size     Line  Section\n";
    }
  }

  const char *printBlanks, *printDashes, *printFormat;
  if (isSymbolList64Bit(Obj)) {
    printBlanks = "                ";
    printDashes = "----------------";
    switch (AddressRadix) {
    case Radix::o:
      printFormat = OutputFormat == posix ? "%" PRIo64 : "%016" PRIo64;
      break;
    case Radix::x:
      printFormat = OutputFormat == posix ? "%" PRIx64 : "%016" PRIx64;
      break;
    default:
      printFormat = OutputFormat == posix ? "%" PRId64 : "%016" PRId64;
    }
  } else {
    printBlanks = "        ";
    printDashes = "--------";
    switch (AddressRadix) {
    case Radix::o:
      printFormat = OutputFormat == posix ? "%" PRIo64 : "%08" PRIo64;
      break;
    case Radix::x:
      printFormat = OutputFormat == posix ? "%" PRIx64 : "%08" PRIx64;
      break;
    default:
      printFormat = OutputFormat == posix ? "%" PRId64 : "%08" PRId64;
    }
  }

  for (const NMSymbol &S : SymbolList) {
    uint32_t SymFlags;
    std::string Name = S.Name;
    MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(&Obj);
    if (Demangle) {
      if (Optional<std::string> Opt = demangle(S.Name, MachO))
        Name = *Opt;
    }
    if (S.Sym.getRawDataRefImpl().p) {
      Expected<uint32_t> SymFlagsOrErr = S.Sym.getFlags();
      if (!SymFlagsOrErr) {
        // TODO: Test this error.
        error(SymFlagsOrErr.takeError(), Obj.getFileName());
        return;
      }
      SymFlags = *SymFlagsOrErr;
    } else
      SymFlags = S.SymFlags;

    bool Undefined = SymFlags & SymbolRef::SF_Undefined;
    bool Global = SymFlags & SymbolRef::SF_Global;
    bool Weak = SymFlags & SymbolRef::SF_Weak;
    bool FormatSpecific = SymFlags & SymbolRef::SF_FormatSpecific;
    if ((!Undefined && UndefinedOnly) || (Undefined && DefinedOnly) ||
        (!Global && ExternalOnly) || (Weak && NoWeakSymbols) ||
        (FormatSpecific && !(SpecialSyms || DebugSyms)))
      continue;
    if (PrintFileName)
      writeFileName(outs(), ArchiveName, ArchitectureName);
    if ((OutputFormat == just_symbols ||
         (UndefinedOnly && MachO && OutputFormat != darwin)) &&
        OutputFormat != posix) {
      outs() << Name << "\n";
      continue;
    }

    char SymbolAddrStr[23], SymbolSizeStr[23];

    // If the format is SysV or the symbol isn't defined, then print spaces.
    if (OutputFormat == sysv || !symbolIsDefined(S)) {
      if (OutputFormat == posix) {
        format(printFormat, S.Address)
            .print(SymbolAddrStr, sizeof(SymbolAddrStr));
        format(printFormat, S.Size).print(SymbolSizeStr, sizeof(SymbolSizeStr));
      } else {
        strcpy(SymbolAddrStr, printBlanks);
        strcpy(SymbolSizeStr, printBlanks);
      }
    }

    if (symbolIsDefined(S)) {
      // Otherwise, print the symbol address and size.
      if (Obj.isIR())
        strcpy(SymbolAddrStr, printDashes);
      else if (MachO && S.TypeChar == 'I')
        strcpy(SymbolAddrStr, printBlanks);
      else
        format(printFormat, S.Address)
            .print(SymbolAddrStr, sizeof(SymbolAddrStr));
      format(printFormat, S.Size).print(SymbolSizeStr, sizeof(SymbolSizeStr));
    }

    // If OutputFormat is darwin or we are printing Mach-O symbols in hex and
    // we have a MachOObjectFile, call darwinPrintSymbol to print as darwin's
    // nm(1) -m output or hex, else if OutputFormat is darwin or we are
    // printing Mach-O symbols in hex and not a Mach-O object fall back to
    // OutputFormat bsd (see below).
    if ((OutputFormat == darwin || FormatMachOasHex) && (MachO || Obj.isIR())) {
      darwinPrintSymbol(Obj, S, SymbolAddrStr, printBlanks, printDashes,
                        printFormat);
    } else if (OutputFormat == posix) {
      outs() << Name << " " << S.TypeChar << " " << SymbolAddrStr << " "
             << (MachO ? "0" : SymbolSizeStr) << "\n";
    } else if (OutputFormat == bsd || (OutputFormat == darwin && !MachO)) {
      if (PrintAddress)
        outs() << SymbolAddrStr << ' ';
      if (PrintSize)
        outs() << SymbolSizeStr << ' ';
      outs() << S.TypeChar;
      if (S.TypeChar == '-' && MachO)
        darwinPrintStab(MachO, S);
      outs() << " " << Name;
      if (S.TypeChar == 'I' && MachO) {
        outs() << " (indirect for ";
        if (S.Sym.getRawDataRefImpl().p) {
          StringRef IndirectName;
          if (MachO->getIndirectName(S.Sym.getRawDataRefImpl(), IndirectName))
            outs() << "?)";
          else
            outs() << IndirectName << ")";
        } else
          outs() << S.IndirectName << ")";
      }
      outs() << "\n";
    } else if (OutputFormat == sysv) {
      outs() << left_justify(Name, 20) << "|" << SymbolAddrStr << "|   "
             << S.TypeChar << "  |" << right_justify(S.TypeName, 18) << "|"
             << SymbolSizeStr << "|     |" << S.SectionName << "\n";
    }
  }

  SymbolList.clear();
}

static char getSymbolNMTypeChar(ELFObjectFileBase &Obj,
                                basic_symbol_iterator I) {
  // OK, this is ELF
  elf_symbol_iterator SymI(I);

  Expected<elf_section_iterator> SecIOrErr = SymI->getSection();
  if (!SecIOrErr) {
    consumeError(SecIOrErr.takeError());
    return '?';
  }

  uint8_t Binding = SymI->getBinding();
  if (Binding == ELF::STB_GNU_UNIQUE)
    return 'u';

  assert(Binding != ELF::STB_WEAK && "STB_WEAK not tested in calling function");
  if (Binding != ELF::STB_GLOBAL && Binding != ELF::STB_LOCAL)
    return '?';

  elf_section_iterator SecI = *SecIOrErr;
  if (SecI != Obj.section_end()) {
    uint32_t Type = SecI->getType();
    uint64_t Flags = SecI->getFlags();
    if (Flags & ELF::SHF_EXECINSTR)
      return 't';
    if (Type == ELF::SHT_NOBITS)
      return 'b';
    if (Flags & ELF::SHF_ALLOC)
      return Flags & ELF::SHF_WRITE ? 'd' : 'r';

    auto NameOrErr = SecI->getName();
    if (!NameOrErr) {
      consumeError(NameOrErr.takeError());
      return '?';
    }
    if ((*NameOrErr).startswith(".debug"))
      return 'N';
    if (!(Flags & ELF::SHF_WRITE))
      return 'n';
  }

  return '?';
}

static char getSymbolNMTypeChar(COFFObjectFile &Obj, symbol_iterator I) {
  COFFSymbolRef Symb = Obj.getCOFFSymbol(*I);
  // OK, this is COFF.
  symbol_iterator SymI(I);

  Expected<StringRef> Name = SymI->getName();
  if (!Name) {
    consumeError(Name.takeError());
    return '?';
  }

  char Ret = StringSwitch<char>(*Name)
                 .StartsWith(".debug", 'N')
                 .StartsWith(".sxdata", 'N')
                 .Default('?');

  if (Ret != '?')
    return Ret;

  uint32_t Characteristics = 0;
  if (!COFF::isReservedSectionNumber(Symb.getSectionNumber())) {
    Expected<section_iterator> SecIOrErr = SymI->getSection();
    if (!SecIOrErr) {
      consumeError(SecIOrErr.takeError());
      return '?';
    }
    section_iterator SecI = *SecIOrErr;
    const coff_section *Section = Obj.getCOFFSection(*SecI);
    Characteristics = Section->Characteristics;
    if (Expected<StringRef> NameOrErr = Obj.getSectionName(Section))
      if (NameOrErr->startswith(".idata"))
        return 'i';
  }

  switch (Symb.getSectionNumber()) {
  case COFF::IMAGE_SYM_DEBUG:
    return 'n';
  default:
    // Check section type.
    if (Characteristics & COFF::IMAGE_SCN_CNT_CODE)
      return 't';
    if (Characteristics & COFF::IMAGE_SCN_CNT_INITIALIZED_DATA)
      return Characteristics & COFF::IMAGE_SCN_MEM_WRITE ? 'd' : 'r';
    if (Characteristics & COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA)
      return 'b';
    if (Characteristics & COFF::IMAGE_SCN_LNK_INFO)
      return 'i';
    // Check for section symbol.
    if (Symb.isSectionDefinition())
      return 's';
  }

  return '?';
}

static char getSymbolNMTypeChar(XCOFFObjectFile &Obj, symbol_iterator I) {
  Expected<uint32_t> TypeOrErr = I->getType();
  if (!TypeOrErr) {
    warn(TypeOrErr.takeError(), Obj.getFileName(),
         "for symbol with index " +
             Twine(Obj.getSymbolIndex(I->getRawDataRefImpl().p)));
    return '?';
  }

  uint32_t SymType = *TypeOrErr;

  if (SymType == SymbolRef::ST_File)
    return 'f';

  // If the I->getSection() call would return an error, the earlier I->getType()
  // call will already have returned the same error first.
  section_iterator SecIter = cantFail(I->getSection());

  if (SecIter == Obj.section_end())
    return '?';

  if (Obj.isDebugSection(SecIter->getRawDataRefImpl()))
    return 'N';

  if (SecIter->isText())
    return 't';

  if (SecIter->isData())
    return 'd';

  if (SecIter->isBSS())
    return 'b';

  return '?';
}

static char getSymbolNMTypeChar(COFFImportFile &Obj) {
  switch (Obj.getCOFFImportHeader()->getType()) {
  case COFF::IMPORT_CODE:
    return 't';
  case COFF::IMPORT_DATA:
    return 'd';
  case COFF::IMPORT_CONST:
    return 'r';
  }
  return '?';
}

static char getSymbolNMTypeChar(MachOObjectFile &Obj, basic_symbol_iterator I) {
  DataRefImpl Symb = I->getRawDataRefImpl();
  uint8_t NType = Obj.is64Bit() ? Obj.getSymbol64TableEntry(Symb).n_type
                                : Obj.getSymbolTableEntry(Symb).n_type;

  if (NType & MachO::N_STAB)
    return '-';

  switch (NType & MachO::N_TYPE) {
  case MachO::N_ABS:
    return 's';
  case MachO::N_INDR:
    return 'i';
  case MachO::N_SECT: {
    Expected<section_iterator> SecOrErr = Obj.getSymbolSection(Symb);
    if (!SecOrErr) {
      consumeError(SecOrErr.takeError());
      return 's';
    }
    section_iterator Sec = *SecOrErr;
    if (Sec == Obj.section_end())
      return 's';
    DataRefImpl Ref = Sec->getRawDataRefImpl();
    StringRef SectionName;
    if (Expected<StringRef> NameOrErr = Obj.getSectionName(Ref))
      SectionName = *NameOrErr;
    StringRef SegmentName = Obj.getSectionFinalSegmentName(Ref);
    if (Obj.is64Bit() && Obj.getHeader64().filetype == MachO::MH_KEXT_BUNDLE &&
        SegmentName == "__TEXT_EXEC" && SectionName == "__text")
      return 't';
    if (SegmentName == "__TEXT" && SectionName == "__text")
      return 't';
    if (SegmentName == "__DATA" && SectionName == "__data")
      return 'd';
    if (SegmentName == "__DATA" && SectionName == "__bss")
      return 'b';
    return 's';
  }
  }

  return '?';
}

static char getSymbolNMTypeChar(TapiFile &Obj, basic_symbol_iterator I) {
  return 's';
}

static char getSymbolNMTypeChar(WasmObjectFile &Obj, basic_symbol_iterator I) {
  uint32_t Flags = cantFail(I->getFlags());
  if (Flags & SymbolRef::SF_Executable)
    return 't';
  return 'd';
}

static char getSymbolNMTypeChar(IRObjectFile &Obj, basic_symbol_iterator I) {
  uint32_t Flags = cantFail(I->getFlags());
  // FIXME: should we print 'b'? At the IR level we cannot be sure if this
  // will be in bss or not, but we could approximate.
  if (Flags & SymbolRef::SF_Executable)
    return 't';
  else if (Triple(Obj.getTargetTriple()).isOSDarwin() &&
           (Flags & SymbolRef::SF_Const))
    return 's';
  else
    return 'd';
}

static bool isObject(SymbolicFile &Obj, basic_symbol_iterator I) {
  return isa<ELFObjectFileBase>(&Obj) &&
         elf_symbol_iterator(I)->getELFType() == ELF::STT_OBJECT;
}

// For ELF object files, Set TypeName to the symbol typename, to be printed
// in the 'Type' column of the SYSV format output.
static StringRef getNMTypeName(SymbolicFile &Obj, basic_symbol_iterator I) {
  if (isa<ELFObjectFileBase>(&Obj)) {
    elf_symbol_iterator SymI(I);
    return SymI->getELFTypeName();
  }
  return "";
}

// Return Posix nm class type tag (single letter), but also set SecName and
// section and name, to be used in format=sysv output.
static char getNMSectionTagAndName(SymbolicFile &Obj, basic_symbol_iterator I,
                                   StringRef &SecName) {
  // Symbol Flags have been checked in the caller.
  uint32_t Symflags = cantFail(I->getFlags());
  if (ELFObjectFileBase *ELFObj = dyn_cast<ELFObjectFileBase>(&Obj)) {
    if (Symflags & object::SymbolRef::SF_Absolute)
      SecName = "*ABS*";
    else if (Symflags & object::SymbolRef::SF_Common)
      SecName = "*COM*";
    else if (Symflags & object::SymbolRef::SF_Undefined)
      SecName = "*UND*";
    else {
      elf_symbol_iterator SymI(I);
      Expected<elf_section_iterator> SecIOrErr = SymI->getSection();
      if (!SecIOrErr) {
        consumeError(SecIOrErr.takeError());
        return '?';
      }

      if (*SecIOrErr == ELFObj->section_end())
        return '?';

      Expected<StringRef> NameOrErr = (*SecIOrErr)->getName();
      if (!NameOrErr) {
        consumeError(NameOrErr.takeError());
        return '?';
      }
      SecName = *NameOrErr;
    }
  }

  if (Symflags & object::SymbolRef::SF_Undefined) {
    if (isa<MachOObjectFile>(Obj) || !(Symflags & object::SymbolRef::SF_Weak))
      return 'U';
    return isObject(Obj, I) ? 'v' : 'w';
  }
  if (isa<ELFObjectFileBase>(&Obj))
    if (ELFSymbolRef(*I).getELFType() == ELF::STT_GNU_IFUNC)
      return 'i';
  if (!isa<MachOObjectFile>(Obj) && (Symflags & object::SymbolRef::SF_Weak))
    return isObject(Obj, I) ? 'V' : 'W';

  if (Symflags & object::SymbolRef::SF_Common)
    return 'C';

  char Ret = '?';
  if (Symflags & object::SymbolRef::SF_Absolute)
    Ret = 'a';
  else if (IRObjectFile *IR = dyn_cast<IRObjectFile>(&Obj))
    Ret = getSymbolNMTypeChar(*IR, I);
  else if (COFFObjectFile *COFF = dyn_cast<COFFObjectFile>(&Obj))
    Ret = getSymbolNMTypeChar(*COFF, I);
  else if (XCOFFObjectFile *XCOFF = dyn_cast<XCOFFObjectFile>(&Obj))
    Ret = getSymbolNMTypeChar(*XCOFF, I);
  else if (COFFImportFile *COFFImport = dyn_cast<COFFImportFile>(&Obj))
    Ret = getSymbolNMTypeChar(*COFFImport);
  else if (MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(&Obj))
    Ret = getSymbolNMTypeChar(*MachO, I);
  else if (WasmObjectFile *Wasm = dyn_cast<WasmObjectFile>(&Obj))
    Ret = getSymbolNMTypeChar(*Wasm, I);
  else if (TapiFile *Tapi = dyn_cast<TapiFile>(&Obj))
    Ret = getSymbolNMTypeChar(*Tapi, I);
  else if (ELFObjectFileBase *ELF = dyn_cast<ELFObjectFileBase>(&Obj)) {
    Ret = getSymbolNMTypeChar(*ELF, I);
    if (ELFSymbolRef(*I).getBinding() == ELF::STB_GNU_UNIQUE)
      return Ret;
  } else
    llvm_unreachable("unknown binary format");

  if (!(Symflags & object::SymbolRef::SF_Global))
    return Ret;

  return toupper(Ret);
}

// getNsectForSegSect() is used to implement the Mach-O "-s segname sectname"
// option to dump only those symbols from that section in a Mach-O file.
// It is called once for each Mach-O file from dumpSymbolNamesFromObject()
// to get the section number for that named section from the command line
// arguments. It returns the section number for that section in the Mach-O
// file or zero it is not present.
static unsigned getNsectForSegSect(MachOObjectFile *Obj) {
  unsigned Nsect = 1;
  for (auto &S : Obj->sections()) {
    DataRefImpl Ref = S.getRawDataRefImpl();
    StringRef SectionName;
    if (Expected<StringRef> NameOrErr = Obj->getSectionName(Ref))
      SectionName = *NameOrErr;
    StringRef SegmentName = Obj->getSectionFinalSegmentName(Ref);
    if (SegmentName == SegSect[0] && SectionName == SegSect[1])
      return Nsect;
    Nsect++;
  }
  return 0;
}

// getNsectInMachO() is used to implement the Mach-O "-s segname sectname"
// option to dump only those symbols from that section in a Mach-O file.
// It is called once for each symbol in a Mach-O file from
// dumpSymbolNamesFromObject() and returns the section number for that symbol
// if it is in a section, else it returns 0.
static unsigned getNsectInMachO(MachOObjectFile &Obj, BasicSymbolRef Sym) {
  DataRefImpl Symb = Sym.getRawDataRefImpl();
  if (Obj.is64Bit()) {
    MachO::nlist_64 STE = Obj.getSymbol64TableEntry(Symb);
    return (STE.n_type & MachO::N_TYPE) == MachO::N_SECT ? STE.n_sect : 0;
  }
  MachO::nlist STE = Obj.getSymbolTableEntry(Symb);
  return (STE.n_type & MachO::N_TYPE) == MachO::N_SECT ? STE.n_sect : 0;
}

static void dumpSymbolsFromDLInfoMachO(MachOObjectFile &MachO) {
  size_t I = SymbolList.size();
  std::string ExportsNameBuffer;
  raw_string_ostream EOS(ExportsNameBuffer);
  std::string BindsNameBuffer;
  raw_string_ostream BOS(BindsNameBuffer);
  std::string LazysNameBuffer;
  raw_string_ostream LOS(LazysNameBuffer);
  std::string WeaksNameBuffer;
  raw_string_ostream WOS(WeaksNameBuffer);
  std::string FunctionStartsNameBuffer;
  raw_string_ostream FOS(FunctionStartsNameBuffer);

  MachO::mach_header H;
  MachO::mach_header_64 H_64;
  uint32_t HFlags = 0;
  if (MachO.is64Bit()) {
    H_64 = MachO.MachOObjectFile::getHeader64();
    HFlags = H_64.flags;
  } else {
    H = MachO.MachOObjectFile::getHeader();
    HFlags = H.flags;
  }
  uint64_t BaseSegmentAddress = 0;
  for (const auto &Command : MachO.load_commands()) {
    if (Command.C.cmd == MachO::LC_SEGMENT) {
      MachO::segment_command Seg = MachO.getSegmentLoadCommand(Command);
      if (Seg.fileoff == 0 && Seg.filesize != 0) {
        BaseSegmentAddress = Seg.vmaddr;
        break;
      }
    } else if (Command.C.cmd == MachO::LC_SEGMENT_64) {
      MachO::segment_command_64 Seg = MachO.getSegment64LoadCommand(Command);
      if (Seg.fileoff == 0 && Seg.filesize != 0) {
        BaseSegmentAddress = Seg.vmaddr;
        break;
      }
    }
  }
  if (DyldInfoOnly || AddDyldInfo ||
      HFlags & MachO::MH_NLIST_OUTOFSYNC_WITH_DYLDINFO) {
    unsigned ExportsAdded = 0;
    Error Err = Error::success();
    for (const llvm::object::ExportEntry &Entry : MachO.exports(Err)) {
      bool found = false;
      bool ReExport = false;
      if (!DyldInfoOnly) {
        for (const NMSymbol &S : SymbolList)
          if (S.Address == Entry.address() + BaseSegmentAddress &&
              S.Name == Entry.name()) {
            found = true;
            break;
          }
      }
      if (!found) {
        NMSymbol S = {};
        S.Address = Entry.address() + BaseSegmentAddress;
        S.Size = 0;
        S.TypeChar = '\0';
        S.Name = Entry.name().str();
        // There is no symbol in the nlist symbol table for this so we set
        // Sym effectivly to null and the rest of code in here must test for
        // it and not do things like Sym.getFlags() for it.
        S.Sym = BasicSymbolRef();
        S.SymFlags = SymbolRef::SF_Global;
        S.Section = SectionRef();
        S.NType = 0;
        S.NSect = 0;
        S.NDesc = 0;

        uint64_t EFlags = Entry.flags();
        bool Abs = ((EFlags & MachO::EXPORT_SYMBOL_FLAGS_KIND_MASK) ==
                    MachO::EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE);
        bool Resolver = (EFlags & MachO::EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER);
        ReExport = (EFlags & MachO::EXPORT_SYMBOL_FLAGS_REEXPORT);
        bool WeakDef = (EFlags & MachO::EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION);
        if (WeakDef)
          S.NDesc |= MachO::N_WEAK_DEF;
        if (Abs) {
          S.NType = MachO::N_EXT | MachO::N_ABS;
          S.TypeChar = 'A';
        } else if (ReExport) {
          S.NType = MachO::N_EXT | MachO::N_INDR;
          S.TypeChar = 'I';
        } else {
          S.NType = MachO::N_EXT | MachO::N_SECT;
          if (Resolver) {
            S.Address = Entry.other() + BaseSegmentAddress;
            if ((S.Address & 1) != 0 && !MachO.is64Bit() &&
                H.cputype == MachO::CPU_TYPE_ARM) {
              S.Address &= ~1LL;
              S.NDesc |= MachO::N_ARM_THUMB_DEF;
            }
          } else {
            S.Address = Entry.address() + BaseSegmentAddress;
          }
          StringRef SegmentName = StringRef();
          StringRef SectionName = StringRef();
          for (const SectionRef &Section : MachO.sections()) {
            S.NSect++;

            if (Expected<StringRef> NameOrErr = Section.getName())
              SectionName = *NameOrErr;
            else
              consumeError(NameOrErr.takeError());

            SegmentName =
                MachO.getSectionFinalSegmentName(Section.getRawDataRefImpl());
            if (S.Address >= Section.getAddress() &&
                S.Address < Section.getAddress() + Section.getSize()) {
              S.Section = Section;
              break;
            } else if (Entry.name() == "__mh_execute_header" &&
                       SegmentName == "__TEXT" && SectionName == "__text") {
              S.Section = Section;
              S.NDesc |= MachO::REFERENCED_DYNAMICALLY;
              break;
            }
          }
          if (SegmentName == "__TEXT" && SectionName == "__text")
            S.TypeChar = 'T';
          else if (SegmentName == "__DATA" && SectionName == "__data")
            S.TypeChar = 'D';
          else if (SegmentName == "__DATA" && SectionName == "__bss")
            S.TypeChar = 'B';
          else
            S.TypeChar = 'S';
        }
        SymbolList.push_back(S);

        EOS << Entry.name();
        EOS << '\0';
        ExportsAdded++;

        // For ReExports there are a two more things to do, first add the
        // indirect name and second create the undefined symbol using the
        // referened dynamic library.
        if (ReExport) {

          // Add the indirect name.
          if (Entry.otherName().empty())
            EOS << Entry.name();
          else
            EOS << Entry.otherName();
          EOS << '\0';

          // Now create the undefined symbol using the referened dynamic
          // library.
          NMSymbol U = {};
          U.Address = 0;
          U.Size = 0;
          U.TypeChar = 'U';
          if (Entry.otherName().empty())
            U.Name = Entry.name().str();
          else
            U.Name = Entry.otherName().str();
          // Again there is no symbol in the nlist symbol table for this so
          // we set Sym effectivly to null and the rest of code in here must
          // test for it and not do things like Sym.getFlags() for it.
          U.Sym = BasicSymbolRef();
          U.SymFlags = SymbolRef::SF_Global | SymbolRef::SF_Undefined;
          U.Section = SectionRef();
          U.NType = MachO::N_EXT | MachO::N_UNDF;
          U.NSect = 0;
          U.NDesc = 0;
          // The library ordinal for this undefined symbol is in the export
          // trie Entry.other().
          MachO::SET_LIBRARY_ORDINAL(U.NDesc, Entry.other());
          SymbolList.push_back(U);

          // Finally add the undefined symbol's name.
          if (Entry.otherName().empty())
            EOS << Entry.name();
          else
            EOS << Entry.otherName();
          EOS << '\0';
          ExportsAdded++;
        }
      }
    }
    if (Err)
      error(std::move(Err), MachO.getFileName());
    // Set the symbol names and indirect names for the added symbols.
    if (ExportsAdded) {
      EOS.flush();
      const char *Q = ExportsNameBuffer.c_str();
      for (unsigned K = 0; K < ExportsAdded; K++) {
        SymbolList[I].Name = Q;
        Q += strlen(Q) + 1;
        if (SymbolList[I].TypeChar == 'I') {
          SymbolList[I].IndirectName = Q;
          Q += strlen(Q) + 1;
        }
        I++;
      }
    }

    // Add the undefined symbols from the bind entries.
    unsigned BindsAdded = 0;
    Error BErr = Error::success();
    StringRef LastSymbolName = StringRef();
    for (const llvm::object::MachOBindEntry &Entry : MachO.bindTable(BErr)) {
      bool found = false;
      if (LastSymbolName == Entry.symbolName())
        found = true;
      else if (!DyldInfoOnly) {
        for (unsigned J = 0; J < SymbolList.size() && !found; ++J) {
          if (SymbolList[J].Name == Entry.symbolName())
            found = true;
        }
      }
      if (!found) {
        LastSymbolName = Entry.symbolName();
        NMSymbol B = {};
        B.Address = 0;
        B.Size = 0;
        B.TypeChar = 'U';
        // There is no symbol in the nlist symbol table for this so we set
        // Sym effectivly to null and the rest of code in here must test for
        // it and not do things like Sym.getFlags() for it.
        B.Sym = BasicSymbolRef();
        B.SymFlags = SymbolRef::SF_Global | SymbolRef::SF_Undefined;
        B.NType = MachO::N_EXT | MachO::N_UNDF;
        B.NSect = 0;
        B.NDesc = 0;
        MachO::SET_LIBRARY_ORDINAL(B.NDesc, Entry.ordinal());
        B.Name = Entry.symbolName().str();
        SymbolList.push_back(B);
        BOS << Entry.symbolName();
        BOS << '\0';
        BindsAdded++;
      }
    }
    if (BErr)
      error(std::move(BErr), MachO.getFileName());
    // Set the symbol names and indirect names for the added symbols.
    if (BindsAdded) {
      BOS.flush();
      const char *Q = BindsNameBuffer.c_str();
      for (unsigned K = 0; K < BindsAdded; K++) {
        SymbolList[I].Name = Q;
        Q += strlen(Q) + 1;
        if (SymbolList[I].TypeChar == 'I') {
          SymbolList[I].IndirectName = Q;
          Q += strlen(Q) + 1;
        }
        I++;
      }
    }

    // Add the undefined symbols from the lazy bind entries.
    unsigned LazysAdded = 0;
    Error LErr = Error::success();
    LastSymbolName = StringRef();
    for (const llvm::object::MachOBindEntry &Entry :
         MachO.lazyBindTable(LErr)) {
      bool found = false;
      if (LastSymbolName == Entry.symbolName())
        found = true;
      else {
        // Here we must check to see it this symbol is already in the
        // SymbolList as it might have already have been added above via a
        // non-lazy (bind) entry.
        for (unsigned J = 0; J < SymbolList.size() && !found; ++J) {
          if (SymbolList[J].Name == Entry.symbolName())
            found = true;
        }
      }
      if (!found) {
        LastSymbolName = Entry.symbolName();
        NMSymbol L = {};
        L.Name = Entry.symbolName().str();
        L.Address = 0;
        L.Size = 0;
        L.TypeChar = 'U';
        // There is no symbol in the nlist symbol table for this so we set
        // Sym effectivly to null and the rest of code in here must test for
        // it and not do things like Sym.getFlags() for it.
        L.Sym = BasicSymbolRef();
        L.SymFlags = SymbolRef::SF_Global | SymbolRef::SF_Undefined;
        L.NType = MachO::N_EXT | MachO::N_UNDF;
        L.NSect = 0;
        // The REFERENCE_FLAG_UNDEFINED_LAZY is no longer used but here it
        // makes sence since we are creating this from a lazy bind entry.
        L.NDesc = MachO::REFERENCE_FLAG_UNDEFINED_LAZY;
        MachO::SET_LIBRARY_ORDINAL(L.NDesc, Entry.ordinal());
        SymbolList.push_back(L);
        LOS << Entry.symbolName();
        LOS << '\0';
        LazysAdded++;
      }
    }
    if (LErr)
      error(std::move(LErr), MachO.getFileName());
    // Set the symbol names and indirect names for the added symbols.
    if (LazysAdded) {
      LOS.flush();
      const char *Q = LazysNameBuffer.c_str();
      for (unsigned K = 0; K < LazysAdded; K++) {
        SymbolList[I].Name = Q;
        Q += strlen(Q) + 1;
        if (SymbolList[I].TypeChar == 'I') {
          SymbolList[I].IndirectName = Q;
          Q += strlen(Q) + 1;
        }
        I++;
      }
    }

    // Add the undefineds symbol from the weak bind entries which are not
    // strong symbols.
    unsigned WeaksAdded = 0;
    Error WErr = Error::success();
    LastSymbolName = StringRef();
    for (const llvm::object::MachOBindEntry &Entry :
         MachO.weakBindTable(WErr)) {
      bool found = false;
      unsigned J = 0;
      if (LastSymbolName == Entry.symbolName() ||
          Entry.flags() & MachO::BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION) {
        found = true;
      } else {
        for (J = 0; J < SymbolList.size() && !found; ++J) {
          if (SymbolList[J].Name == Entry.symbolName()) {
            found = true;
            break;
          }
        }
      }
      if (!found) {
        LastSymbolName = Entry.symbolName();
        NMSymbol W = {};
        W.Name = Entry.symbolName().str();
        W.Address = 0;
        W.Size = 0;
        W.TypeChar = 'U';
        // There is no symbol in the nlist symbol table for this so we set
        // Sym effectivly to null and the rest of code in here must test for
        // it and not do things like Sym.getFlags() for it.
        W.Sym = BasicSymbolRef();
        W.SymFlags = SymbolRef::SF_Global | SymbolRef::SF_Undefined;
        W.NType = MachO::N_EXT | MachO::N_UNDF;
        W.NSect = 0;
        // Odd that we are using N_WEAK_DEF on an undefined symbol but that is
        // what is created in this case by the linker when there are real
        // symbols in the nlist structs.
        W.NDesc = MachO::N_WEAK_DEF;
        SymbolList.push_back(W);
        WOS << Entry.symbolName();
        WOS << '\0';
        WeaksAdded++;
      } else {
        // This is the case the symbol was previously been found and it could
        // have been added from a bind or lazy bind symbol.  If so and not
        // a definition also mark it as weak.
        if (SymbolList[J].TypeChar == 'U')
          // See comment above about N_WEAK_DEF.
          SymbolList[J].NDesc |= MachO::N_WEAK_DEF;
      }
    }
    if (WErr)
      error(std::move(WErr), MachO.getFileName());
    // Set the symbol names and indirect names for the added symbols.
    if (WeaksAdded) {
      WOS.flush();
      const char *Q = WeaksNameBuffer.c_str();
      for (unsigned K = 0; K < WeaksAdded; K++) {
        SymbolList[I].Name = Q;
        Q += strlen(Q) + 1;
        if (SymbolList[I].TypeChar == 'I') {
          SymbolList[I].IndirectName = Q;
          Q += strlen(Q) + 1;
        }
        I++;
      }
    }

    // Trying adding symbol from the function starts table and LC_MAIN entry
    // point.
    SmallVector<uint64_t, 8> FoundFns;
    uint64_t lc_main_offset = UINT64_MAX;
    for (const auto &Command : MachO.load_commands()) {
      if (Command.C.cmd == MachO::LC_FUNCTION_STARTS) {
        // We found a function starts segment, parse the addresses for
        // consumption.
        MachO::linkedit_data_command LLC =
            MachO.getLinkeditDataLoadCommand(Command);

        MachO.ReadULEB128s(LLC.dataoff, FoundFns);
      } else if (Command.C.cmd == MachO::LC_MAIN) {
        MachO::entry_point_command LCmain = MachO.getEntryPointCommand(Command);
        lc_main_offset = LCmain.entryoff;
      }
    }
    // See if these addresses are already in the symbol table.
    unsigned FunctionStartsAdded = 0;
    for (uint64_t f = 0; f < FoundFns.size(); f++) {
      bool found = false;
      for (unsigned J = 0; J < SymbolList.size() && !found; ++J) {
        if (SymbolList[J].Address == FoundFns[f] + BaseSegmentAddress)
          found = true;
      }
      // See this address is not already in the symbol table fake up an
      // nlist for it.
      if (!found) {
        NMSymbol F = {};
        F.Name = "<redacted function X>";
        F.Address = FoundFns[f] + BaseSegmentAddress;
        F.Size = 0;
        // There is no symbol in the nlist symbol table for this so we set
        // Sym effectivly to null and the rest of code in here must test for
        // it and not do things like Sym.getFlags() for it.
        F.Sym = BasicSymbolRef();
        F.SymFlags = 0;
        F.NType = MachO::N_SECT;
        F.NSect = 0;
        StringRef SegmentName = StringRef();
        StringRef SectionName = StringRef();
        for (const SectionRef &Section : MachO.sections()) {
          if (Expected<StringRef> NameOrErr = Section.getName())
            SectionName = *NameOrErr;
          else
            consumeError(NameOrErr.takeError());

          SegmentName =
              MachO.getSectionFinalSegmentName(Section.getRawDataRefImpl());
          F.NSect++;
          if (F.Address >= Section.getAddress() &&
              F.Address < Section.getAddress() + Section.getSize()) {
            F.Section = Section;
            break;
          }
        }
        if (SegmentName == "__TEXT" && SectionName == "__text")
          F.TypeChar = 't';
        else if (SegmentName == "__DATA" && SectionName == "__data")
          F.TypeChar = 'd';
        else if (SegmentName == "__DATA" && SectionName == "__bss")
          F.TypeChar = 'b';
        else
          F.TypeChar = 's';
        F.NDesc = 0;
        SymbolList.push_back(F);
        if (FoundFns[f] == lc_main_offset)
          FOS << "<redacted LC_MAIN>";
        else
          FOS << "<redacted function " << f << ">";
        FOS << '\0';
        FunctionStartsAdded++;
      }
    }
    if (FunctionStartsAdded) {
      FOS.flush();
      const char *Q = FunctionStartsNameBuffer.c_str();
      for (unsigned K = 0; K < FunctionStartsAdded; K++) {
        SymbolList[I].Name = Q;
        Q += strlen(Q) + 1;
        if (SymbolList[I].TypeChar == 'I') {
          SymbolList[I].IndirectName = Q;
          Q += strlen(Q) + 1;
        }
        I++;
      }
    }
  }
}

static void dumpSymbolNamesFromObject(SymbolicFile &Obj, bool printName,
                                      StringRef ArchiveName = {},
                                      StringRef ArchitectureName = {}) {
  auto Symbols = Obj.symbols();
  std::vector<VersionEntry> SymbolVersions;
  if (DynamicSyms) {
    const auto *E = dyn_cast<ELFObjectFileBase>(&Obj);
    if (!E) {
      error("File format has no dynamic symbol table", Obj.getFileName());
      return;
    }
    Symbols = E->getDynamicSymbolIterators();

    if (Expected<std::vector<VersionEntry>> VersionsOrErr =
            E->readDynsymVersions())
      SymbolVersions = std::move(*VersionsOrErr);
    else
      WithColor::warning(errs(), ToolName)
          << "unable to read symbol versions: "
          << toString(VersionsOrErr.takeError()) << "\n";
  }

  // If a "-s segname sectname" option was specified and this is a Mach-O
  // file get the section number for that section in this object file.
  unsigned int Nsect = 0;
  MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(&Obj);
  if (!SegSect.empty() && MachO) {
    Nsect = getNsectForSegSect(MachO);
    // If this section is not in the object file no symbols are printed.
    if (Nsect == 0)
      return;
  }
  if (!(MachO && DyldInfoOnly)) {
    size_t I = -1;
    for (BasicSymbolRef Sym : Symbols) {
      ++I;
      Expected<uint32_t> SymFlagsOrErr = Sym.getFlags();
      if (!SymFlagsOrErr) {
        error(SymFlagsOrErr.takeError(), Obj.getFileName());
        return;
      }

      // Don't drop format specifc symbols for ARM and AArch64 ELF targets, they
      // are used to repesent mapping symbols and needed to honor the
      // --special-syms option.
      auto *ELFObj = dyn_cast<ELFObjectFileBase>(&Obj);
      if ((!ELFObj || (ELFObj->getEMachine() != ELF::EM_ARM &&
                       ELFObj->getEMachine() != ELF::EM_AARCH64)) &&
          !DebugSyms && (*SymFlagsOrErr & SymbolRef::SF_FormatSpecific))
        continue;
      if (WithoutAliases && (*SymFlagsOrErr & SymbolRef::SF_Indirect))
        continue;
      // If a "-s segname sectname" option was specified and this is a Mach-O
      // file and this section appears in this file, Nsect will be non-zero then
      // see if this symbol is a symbol from that section and if not skip it.
      if (Nsect && Nsect != getNsectInMachO(*MachO, Sym))
        continue;
      NMSymbol S = {};
      S.Size = 0;
      S.Address = 0;
      if (isa<ELFObjectFileBase>(&Obj))
        S.Size = ELFSymbolRef(Sym).getSize();

      if (const XCOFFObjectFile *XCOFFObj =
              dyn_cast<const XCOFFObjectFile>(&Obj))
        S.Size = XCOFFObj->getSymbolSize(Sym.getRawDataRefImpl());

      if (PrintAddress && isa<ObjectFile>(Obj)) {
        SymbolRef SymRef(Sym);
        Expected<uint64_t> AddressOrErr = SymRef.getAddress();
        if (!AddressOrErr) {
          consumeError(AddressOrErr.takeError());
          break;
        }
        S.Address = *AddressOrErr;
      }
      S.TypeName = getNMTypeName(Obj, Sym);
      S.TypeChar = getNMSectionTagAndName(Obj, Sym, S.SectionName);

      raw_string_ostream OS(S.Name);
      if (Error E = Sym.printName(OS)) {
        if (MachO) {
          OS << "bad string index";
          consumeError(std::move(E));
        } else
          error(std::move(E), Obj.getFileName());
      }
      if (!SymbolVersions.empty() && !SymbolVersions[I].Name.empty())
        S.Name +=
            (SymbolVersions[I].IsVerDef ? "@@" : "@") + SymbolVersions[I].Name;

      S.Sym = Sym;
      SymbolList.push_back(S);
    }
  }

  // If this is a Mach-O file where the nlist symbol table is out of sync
  // with the dyld export trie then look through exports and fake up symbols
  // for the ones that are missing (also done with the -add-dyldinfo flag).
  // This is needed if strip(1) -T is run on a binary containing swift
  // language symbols for example.  The option -only-dyldinfo will fake up
  // all symbols from the dyld export trie as well as the bind info.
  if (MachO && !NoDyldInfo)
    dumpSymbolsFromDLInfoMachO(*MachO);

  CurrentFilename = Obj.getFileName();

  if (Symbols.empty() && SymbolList.empty() && !Quiet) {
    writeFileName(errs(), ArchiveName, ArchitectureName);
    errs() << "no symbols\n";
  }

  sortAndPrintSymbolList(Obj, printName, ArchiveName, ArchitectureName);
}

// checkMachOAndArchFlags() checks to see if the SymbolicFile is a Mach-O file
// and if it is and there is a list of architecture flags is specified then
// check to make sure this Mach-O file is one of those architectures or all
// architectures was specificed.  If not then an error is generated and this
// routine returns false.  Else it returns true.
static bool checkMachOAndArchFlags(SymbolicFile *O, std::string &Filename) {
  auto *MachO = dyn_cast<MachOObjectFile>(O);

  if (!MachO || ArchAll || ArchFlags.empty())
    return true;

  MachO::mach_header H;
  MachO::mach_header_64 H_64;
  Triple T;
  const char *McpuDefault, *ArchFlag;
  if (MachO->is64Bit()) {
    H_64 = MachO->MachOObjectFile::getHeader64();
    T = MachOObjectFile::getArchTriple(H_64.cputype, H_64.cpusubtype,
                                       &McpuDefault, &ArchFlag);
  } else {
    H = MachO->MachOObjectFile::getHeader();
    T = MachOObjectFile::getArchTriple(H.cputype, H.cpusubtype,
                                       &McpuDefault, &ArchFlag);
  }
  const std::string ArchFlagName(ArchFlag);
  if (!llvm::is_contained(ArchFlags, ArchFlagName)) {
    error("No architecture specified", Filename);
    return false;
  }
  return true;
}

static void dumpSymbolNamesFromFile(std::string &Filename) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> BufferOrErr =
      MemoryBuffer::getFileOrSTDIN(Filename);
  if (error(BufferOrErr.getError(), Filename))
    return;

  LLVMContext Context;
  LLVMContext *ContextPtr = NoLLVMBitcode ? nullptr : &Context;
  Expected<std::unique_ptr<Binary>> BinaryOrErr =
      createBinary(BufferOrErr.get()->getMemBufferRef(), ContextPtr);
  if (!BinaryOrErr) {
    error(BinaryOrErr.takeError(), Filename);
    return;
  }
  Binary &Bin = *BinaryOrErr.get();

  if (Archive *A = dyn_cast<Archive>(&Bin)) {
    if (ArchiveMap) {
      Archive::symbol_iterator I = A->symbol_begin();
      Archive::symbol_iterator E = A->symbol_end();
      if (I != E) {
        outs() << "Archive map\n";
        for (; I != E; ++I) {
          Expected<Archive::Child> C = I->getMember();
          if (!C) {
            error(C.takeError(), Filename);
            break;
          }
          Expected<StringRef> FileNameOrErr = C->getName();
          if (!FileNameOrErr) {
            error(FileNameOrErr.takeError(), Filename);
            break;
          }
          StringRef SymName = I->getName();
          outs() << SymName << " in " << FileNameOrErr.get() << "\n";
        }
        outs() << "\n";
      }
    }

    {
      Error Err = Error::success();
      for (auto &C : A->children(Err)) {
        Expected<std::unique_ptr<Binary>> ChildOrErr =
            C.getAsBinary(ContextPtr);
        if (!ChildOrErr) {
          if (auto E = isNotObjectErrorInvalidFileType(ChildOrErr.takeError()))
            error(std::move(E), Filename, C);
          continue;
        }
        if (SymbolicFile *O = dyn_cast<SymbolicFile>(&*ChildOrErr.get())) {
          if (!MachOPrintSizeWarning && PrintSize &&  isa<MachOObjectFile>(O)) {
            WithColor::warning(errs(), ToolName)
                << "sizes with -print-size for Mach-O files are always zero.\n";
            MachOPrintSizeWarning = true;
          }
          if (!checkMachOAndArchFlags(O, Filename))
            return;
          if (!PrintFileName) {
            outs() << "\n";
            if (isa<MachOObjectFile>(O)) {
              outs() << Filename << "(" << O->getFileName() << ")";
            } else
              outs() << O->getFileName();
            outs() << ":\n";
          }
          dumpSymbolNamesFromObject(*O, false, Filename);
        }
      }
      if (Err)
        error(std::move(Err), A->getFileName());
    }
    return;
  }
  if (MachOUniversalBinary *UB = dyn_cast<MachOUniversalBinary>(&Bin)) {
    // If we have a list of architecture flags specified dump only those.
    if (!ArchAll && !ArchFlags.empty()) {
      // Look for a slice in the universal binary that matches each ArchFlag.
      bool ArchFound;
      for (unsigned i = 0; i < ArchFlags.size(); ++i) {
        ArchFound = false;
        for (MachOUniversalBinary::object_iterator I = UB->begin_objects(),
                                                   E = UB->end_objects();
             I != E; ++I) {
          if (ArchFlags[i] == I->getArchFlagName()) {
            ArchFound = true;
            Expected<std::unique_ptr<ObjectFile>> ObjOrErr =
                I->getAsObjectFile();
            std::string ArchiveName;
            std::string ArchitectureName;
            ArchiveName.clear();
            ArchitectureName.clear();
            if (ObjOrErr) {
              ObjectFile &Obj = *ObjOrErr.get();
              if (ArchFlags.size() > 1) {
                if (PrintFileName)
                  ArchitectureName = I->getArchFlagName();
                else
                  outs() << "\n" << Obj.getFileName() << " (for architecture "
                         << I->getArchFlagName() << ")"
                         << ":\n";
              }
              dumpSymbolNamesFromObject(Obj, false, ArchiveName,
                                        ArchitectureName);
            } else if (auto E = isNotObjectErrorInvalidFileType(
                       ObjOrErr.takeError())) {
              error(std::move(E), Filename, ArchFlags.size() > 1 ?
                    StringRef(I->getArchFlagName()) : StringRef());
              continue;
            } else if (Expected<std::unique_ptr<Archive>> AOrErr =
                           I->getAsArchive()) {
              std::unique_ptr<Archive> &A = *AOrErr;
              Error Err = Error::success();
              for (auto &C : A->children(Err)) {
                Expected<std::unique_ptr<Binary>> ChildOrErr =
                    C.getAsBinary(ContextPtr);
                if (!ChildOrErr) {
                  if (auto E = isNotObjectErrorInvalidFileType(
                                       ChildOrErr.takeError())) {
                    error(std::move(E), Filename, C, ArchFlags.size() > 1 ?
                          StringRef(I->getArchFlagName()) : StringRef());
                  }
                  continue;
                }
                if (SymbolicFile *O =
                        dyn_cast<SymbolicFile>(&*ChildOrErr.get())) {
                  if (PrintFileName) {
                    ArchiveName = std::string(A->getFileName());
                    if (ArchFlags.size() > 1)
                      ArchitectureName = I->getArchFlagName();
                  } else {
                    outs() << "\n" << A->getFileName();
                    outs() << "(" << O->getFileName() << ")";
                    if (ArchFlags.size() > 1) {
                      outs() << " (for architecture " << I->getArchFlagName()
                             << ")";
                    }
                    outs() << ":\n";
                  }
                  dumpSymbolNamesFromObject(*O, false, ArchiveName,
                                            ArchitectureName);
                }
              }
              if (Err)
                error(std::move(Err), A->getFileName());
            } else {
              consumeError(AOrErr.takeError());
              error(Filename + " for architecture " +
                    StringRef(I->getArchFlagName()) +
                    " is not a Mach-O file or an archive file",
                    "Mach-O universal file");
            }
          }
        }
        if (!ArchFound) {
          error(ArchFlags[i],
                "file: " + Filename + " does not contain architecture");
          return;
        }
      }
      return;
    }
    // No architecture flags were specified so if this contains a slice that
    // matches the host architecture dump only that.
    if (!ArchAll) {
      Triple HostTriple = MachOObjectFile::getHostArch();
      StringRef HostArchName = HostTriple.getArchName();
      for (MachOUniversalBinary::object_iterator I = UB->begin_objects(),
                                                 E = UB->end_objects();
           I != E; ++I) {
        if (HostArchName == I->getArchFlagName()) {
          Expected<std::unique_ptr<ObjectFile>> ObjOrErr = I->getAsObjectFile();
          std::string ArchiveName;
          if (ObjOrErr) {
            ObjectFile &Obj = *ObjOrErr.get();
            dumpSymbolNamesFromObject(Obj, false);
          } else if (auto E = isNotObjectErrorInvalidFileType(
                     ObjOrErr.takeError())) {
            error(std::move(E), Filename);
            return;
          } else if (Expected<std::unique_ptr<Archive>> AOrErr =
                         I->getAsArchive()) {
            std::unique_ptr<Archive> &A = *AOrErr;
            Error Err = Error::success();
            for (auto &C : A->children(Err)) {
              Expected<std::unique_ptr<Binary>> ChildOrErr =
                  C.getAsBinary(ContextPtr);
              if (!ChildOrErr) {
                if (auto E = isNotObjectErrorInvalidFileType(
                                     ChildOrErr.takeError()))
                  error(std::move(E), Filename, C);
                continue;
              }
              if (SymbolicFile *O =
                      dyn_cast<SymbolicFile>(&*ChildOrErr.get())) {
                if (PrintFileName)
                  ArchiveName = std::string(A->getFileName());
                else
                  outs() << "\n" << A->getFileName() << "(" << O->getFileName()
                         << ")"
                         << ":\n";
                dumpSymbolNamesFromObject(*O, false, ArchiveName);
              }
            }
            if (Err)
              error(std::move(Err), A->getFileName());
          } else {
            consumeError(AOrErr.takeError());
            error(Filename + " for architecture " +
                  StringRef(I->getArchFlagName()) +
                  " is not a Mach-O file or an archive file",
                  "Mach-O universal file");
          }
          return;
        }
      }
    }
    // Either all architectures have been specified or none have been specified
    // and this does not contain the host architecture so dump all the slices.
    bool moreThanOneArch = UB->getNumberOfObjects() > 1;
    for (const MachOUniversalBinary::ObjectForArch &O : UB->objects()) {
      Expected<std::unique_ptr<ObjectFile>> ObjOrErr = O.getAsObjectFile();
      std::string ArchiveName;
      std::string ArchitectureName;
      ArchiveName.clear();
      ArchitectureName.clear();
      if (ObjOrErr) {
        ObjectFile &Obj = *ObjOrErr.get();
        if (PrintFileName) {
          if (isa<MachOObjectFile>(Obj) && moreThanOneArch)
            ArchitectureName = O.getArchFlagName();
        } else {
          if (moreThanOneArch)
            outs() << "\n";
          outs() << Obj.getFileName();
          if (isa<MachOObjectFile>(Obj) && moreThanOneArch)
            outs() << " (for architecture " << O.getArchFlagName() << ")";
          outs() << ":\n";
        }
        dumpSymbolNamesFromObject(Obj, false, ArchiveName, ArchitectureName);
      } else if (auto E = isNotObjectErrorInvalidFileType(
                 ObjOrErr.takeError())) {
        error(std::move(E), Filename, moreThanOneArch ?
              StringRef(O.getArchFlagName()) : StringRef());
        continue;
      } else if (Expected<std::unique_ptr<Archive>> AOrErr =
                  O.getAsArchive()) {
        std::unique_ptr<Archive> &A = *AOrErr;
        Error Err = Error::success();
        for (auto &C : A->children(Err)) {
          Expected<std::unique_ptr<Binary>> ChildOrErr =
            C.getAsBinary(ContextPtr);
          if (!ChildOrErr) {
            if (auto E = isNotObjectErrorInvalidFileType(
                                 ChildOrErr.takeError()))
              error(std::move(E), Filename, C, moreThanOneArch ?
                    StringRef(ArchitectureName) : StringRef());
            continue;
          }
          if (SymbolicFile *F = dyn_cast<SymbolicFile>(&*ChildOrErr.get())) {
            if (PrintFileName) {
              ArchiveName = std::string(A->getFileName());
              if (isa<MachOObjectFile>(F) && moreThanOneArch)
                ArchitectureName = O.getArchFlagName();
            } else {
              outs() << "\n" << A->getFileName();
              if (isa<MachOObjectFile>(F)) {
                outs() << "(" << F->getFileName() << ")";
                if (moreThanOneArch)
                  outs() << " (for architecture " << O.getArchFlagName()
                         << ")";
              } else
                outs() << ":" << F->getFileName();
              outs() << ":\n";
            }
            dumpSymbolNamesFromObject(*F, false, ArchiveName, ArchitectureName);
          }
        }
        if (Err)
          error(std::move(Err), A->getFileName());
      } else {
        consumeError(AOrErr.takeError());
        error(Filename + " for architecture " +
              StringRef(O.getArchFlagName()) +
              " is not a Mach-O file or an archive file",
              "Mach-O universal file");
      }
    }
    return;
  }

  if (TapiUniversal *TU = dyn_cast<TapiUniversal>(&Bin)) {
    for (const TapiUniversal::ObjectForArch &I : TU->objects()) {
      StringRef ArchName = I.getArchFlagName();
      const bool ShowArch =
          ArchFlags.empty() || llvm::is_contained(ArchFlags, ArchName);
      if (!ShowArch)
        continue;
      if (!AddInlinedInfo && !I.isTopLevelLib())
        continue;
      if (auto ObjOrErr = I.getAsObjectFile()) {
        outs() << "\n"
               << I.getInstallName() << " (for architecture " << ArchName << ")"
               << ":\n";
        dumpSymbolNamesFromObject(*ObjOrErr.get(), false, {}, ArchName);
      } else if (Error E =
                     isNotObjectErrorInvalidFileType(ObjOrErr.takeError())) {
        error(std::move(E), Filename, ArchName);
      }
    }

    return;
  }

  if (SymbolicFile *O = dyn_cast<SymbolicFile>(&Bin)) {
    if (!MachOPrintSizeWarning && PrintSize &&  isa<MachOObjectFile>(O)) {
      WithColor::warning(errs(), ToolName)
          << "sizes with --print-size for Mach-O files are always zero.\n";
      MachOPrintSizeWarning = true;
    }
    if (!checkMachOAndArchFlags(O, Filename))
      return;
    dumpSymbolNamesFromObject(*O, true);
  }
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  BumpPtrAllocator A;
  StringSaver Saver(A);
  NmOptTable Tbl;
  ToolName = argv[0];
  opt::InputArgList Args =
      Tbl.parseArgs(argc, argv, OPT_UNKNOWN, Saver, [&](StringRef Msg) {
        error(Msg);
        exit(1);
      });
  if (Args.hasArg(OPT_help)) {
    Tbl.printHelp(
        outs(),
        (Twine(ToolName) + " [options] <input object files>").str().c_str(),
        "LLVM symbol table dumper");
    // TODO Replace this with OptTable API once it adds extrahelp support.
    outs() << "\nPass @FILE as argument to read options from FILE.\n";
    return 0;
  }
  if (Args.hasArg(OPT_version)) {
    // This needs to contain the word "GNU", libtool looks for that string.
    outs() << "llvm-nm, compatible with GNU nm" << '\n';
    cl::PrintVersionMessage();
    return 0;
  }

  DebugSyms = Args.hasArg(OPT_debug_syms);
  DefinedOnly = Args.hasArg(OPT_defined_only);
  Demangle = Args.hasFlag(OPT_demangle, OPT_no_demangle, false);
  DynamicSyms = Args.hasArg(OPT_dynamic);
  ExternalOnly = Args.hasArg(OPT_extern_only);
  StringRef V = Args.getLastArgValue(OPT_format_EQ, "bsd");
  if (V == "bsd")
    OutputFormat = bsd;
  else if (V == "posix")
    OutputFormat = posix;
  else if (V == "sysv")
    OutputFormat = sysv;
  else if (V == "darwin")
    OutputFormat = darwin;
  else if (V == "just-symbols")
    OutputFormat = just_symbols;
  else
    error("--format value should be one of: bsd, posix, sysv, darwin, "
          "just-symbols");
  NoLLVMBitcode = Args.hasArg(OPT_no_llvm_bc);
  NoSort = Args.hasArg(OPT_no_sort);
  NoWeakSymbols = Args.hasArg(OPT_no_weak);
  NumericSort = Args.hasArg(OPT_numeric_sort);
  ArchiveMap = Args.hasArg(OPT_print_armap);
  PrintFileName = Args.hasArg(OPT_print_file_name);
  PrintSize = Args.hasArg(OPT_print_size);
  ReverseSort = Args.hasArg(OPT_reverse_sort);
  Quiet = Args.hasArg(OPT_quiet);
  V = Args.getLastArgValue(OPT_radix_EQ, "x");
  if (V == "o")
    AddressRadix = Radix::o;
  else if (V == "d")
    AddressRadix = Radix::d;
  else if (V == "x")
    AddressRadix = Radix::x;
  else
    error("--radix value should be one of: 'o' (octal), 'd' (decimal), 'x' "
          "(hexadecimal)");
  SizeSort = Args.hasArg(OPT_size_sort);
  SpecialSyms = Args.hasArg(OPT_special_syms);
  UndefinedOnly = Args.hasArg(OPT_undefined_only);
  WithoutAliases = Args.hasArg(OPT_without_aliases);

  // Mach-O specific options.
  FormatMachOasHex = Args.hasArg(OPT_x);
  AddDyldInfo = Args.hasArg(OPT_add_dyldinfo);
  AddInlinedInfo = Args.hasArg(OPT_add_inlinedinfo);
  DyldInfoOnly = Args.hasArg(OPT_dyldinfo_only);
  NoDyldInfo = Args.hasArg(OPT_no_dyldinfo);

  // llvm-nm only reads binary files.
  if (error(sys::ChangeStdinToBinary()))
    return 1;

  // These calls are needed so that we can read bitcode correctly.
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();

  // The relative order of these is important. If you pass --size-sort it should
  // only print out the size. However, if you pass -S --size-sort, it should
  // print out both the size and address.
  if (SizeSort && !PrintSize)
    PrintAddress = false;
  if (OutputFormat == sysv || SizeSort)
    PrintSize = true;

  for (const auto *A : Args.filtered(OPT_arch_EQ)) {
    SmallVector<StringRef, 2> Values;
    llvm::SplitString(A->getValue(), Values, ",");
    for (StringRef V : Values) {
      if (V == "all")
        ArchAll = true;
      else if (MachOObjectFile::isValidArch(V))
        ArchFlags.push_back(V);
      else
        error("Unknown architecture named '" + V + "'",
              "for the --arch option");
    }
  }

  // Mach-O takes -s to accept two arguments. We emulate this by iterating over
  // both OPT_s and OPT_INPUT.
  std::vector<std::string> InputFilenames;
  int SegSectArgs = 0;
  for (opt::Arg *A : Args.filtered(OPT_s, OPT_INPUT)) {
    if (SegSectArgs > 0) {
      --SegSectArgs;
      SegSect.push_back(A->getValue());
    } else if (A->getOption().matches(OPT_s)) {
      SegSectArgs = 2;
    } else {
      InputFilenames.push_back(A->getValue());
    }
  }
  if (!SegSect.empty() && SegSect.size() != 2)
    error("bad number of arguments (must be two arguments)",
          "for the -s option");

  if (InputFilenames.empty())
    InputFilenames.push_back("a.out");
  if (InputFilenames.size() > 1)
    MultipleFiles = true;

  if (NoDyldInfo && (AddDyldInfo || DyldInfoOnly))
    error("--no-dyldinfo can't be used with --add-dyldinfo or --dyldinfo-only");

  llvm::for_each(InputFilenames, dumpSymbolNamesFromFile);

  if (HadError)
    return 1;
}
