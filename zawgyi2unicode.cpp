/*
 * Part of the Parabix Project, under the Open Software License 3.0.
 * SPDX-License-Identifier: OSL-3.0
 */

#include <cstddef>
#include <fcntl.h>
#include <string>
#include <vector>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <pablo/codegenstate.h>
#include <pablo/pe_zeroes.h>
#include <pablo/pe_ones.h>
#include <pablo/bixnum/bixnum.h>
#include <kernel/core/kernel_builder.h>
#include <kernel/pipeline/program_builder.h>
#include <kernel/streamutils/deletion.h>
#include <kernel/streamutils/pdep_kernel.h>
#include <kernel/streamutils/string_insert.h>
#include <kernel/basis/s2p_kernel.h>
#include <kernel/basis/p2s_kernel.h>
#include <kernel/io/source_kernel.h>
#include <kernel/io/stdout_kernel.h>
#include <kernel/unicode/charclasses.h>
#include <kernel/unicode/utf8_support.h>
#include <kernel/unicode/utf8gen.h>
#include <kernel/unicode/utf8_decoder.h>
#include <toolchain/toolchain.h>
#include <pablo/pablo_toolchain.h>
#include <kernel/pipeline/driver/cpudriver.h>
#include <re/toolchain/toolchain.h>

using namespace kernel;
using namespace llvm;
using namespace pablo;

static cl::OptionCategory ZawgyiOptions("Zawgyi Transcoder Options", "Zawgyi Transcoder Options.");
static cl::opt<std::string> inputFile(cl::Positional, cl::desc("<input file>"), cl::Required, cl::cat(ZawgyiOptions));

#define SHOW_STREAM(name) if (codegen::EnableIllustrator) P.captureBitstream(#name, name)
#define SHOW_BIXNUM(name) if (codegen::EnableIllustrator) P.captureBixNum(#name, name)
#define SHOW_BYTES(name) if (codegen::EnableIllustrator) P.captureByteData(#name, name)

std::vector<PabloAST*> createConstantBixNum(PabloBuilder & pb, uint64_t value, unsigned size) {
    std::vector<PabloAST*> b(size);
    for (unsigned i = 0; i < size; ++i) {
        if ((value >> i) & 1) {
            b[i] = pb.createOnes();
        } else {
            b[i] = pb.createZeroes();
        }
    }
    return b;
}

// =========================================================================
// Helper: Create a mask for a list of codepoints
// =========================================================================
PabloAST * createCodepointMask(PabloBuilder & pb, BixNumCompiler & bnc, 
                               const std::vector<PabloAST *> & basis, 
                               const std::vector<uint32_t> & codepoints) {
    PabloAST * mask = pb.createZeroes();
    for (uint32_t cp : codepoints) {
        mask = pb.createOr(mask, bnc.EQ(basis, cp));
    }
    return mask;
}

// =========================================================================
// STAGE 3: Classifier - Detects ALL Zawgyi patterns that need expansion
// =========================================================================
class ZawgyiClassifier : public pablo::PabloKernel {
public:
    ZawgyiClassifier(LLVMTypeSystemInterface & ts, StreamSet * U21, 
                     StreamSet * ZawgyiMask, StreamSet * LigatureCounts);
protected:
    void generatePabloMethod() override;
};

ZawgyiClassifier::ZawgyiClassifier(LLVMTypeSystemInterface & ts, StreamSet * U21, 
                                   StreamSet * ZawgyiMask, StreamSet * LigatureCounts)
: PabloKernel(ts, "ZawgyiClassifier", 
  {Binding{"U21", U21}}, 
  {Binding{"ZawgyiMask", ZawgyiMask}, Binding{"LigatureCounts", LigatureCounts}}) {}

void ZawgyiClassifier::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> basis = getInputStreamSet("U21");
    BixNumCompiler bnc(pb);

    // 1. Core Classification Masks
    PabloAST * isMyanmarBlock = pb.createAnd(bnc.UGE(basis, 0x1000), bnc.ULE(basis, 0x109F));
    PabloAST * isZawgyiSpecial = pb.createOr(bnc.EQ(basis, 0x105A), bnc.EQ(basis, 0x1060));
    PabloAST * zawgyiMask = pb.createOr(isMyanmarBlock, isZawgyiSpecial);
    writeOutputStreamSet("ZawgyiMask", std::vector<PabloAST*>{ zawgyiMask });

    // 2. Ligature Expansion Counting
    // All codepoints that expand to 2 characters
    std::vector<uint32_t> expand2_codepoints = {
        // Tall AA
        0x105A,
        // Stacked consonants (Zawgyi -> Unicode1 + Unicode2)
        0x1060, 0x1061, 0x1062, 0x1063, 0x1065, 0x1066, 0x1067,
        0x1068, 0x1069, 0x106C, 0x106D, 0x1070, 0x1071, 0x1072,
        0x1073, 0x1074, 0x1075, 0x1076, 0x1077, 0x1078, 0x1079,
        0x107A, 0x107B, 0x107C, 0x1085, 0x1093, 0x1088, 0x1089
    };
    
    // All codepoints that expand to 3 characters
    std::vector<uint32_t> expand3_codepoints = {
        0x1096   // -> 0x1039 + 0x1010 + 0x103D
    };
    
    PabloAST * expand2 = createCodepointMask(pb, bnc, basis, expand2_codepoints);
    PabloAST * expand3 = createCodepointMask(pb, bnc, basis, expand3_codepoints);
    
    // LigatureCounts is a 2-bit BixNum: 
    // 01 = expand to 2 chars, 10 = expand to 3 chars
    std::vector<Var *> expansionCount(2);
    expansionCount[0] = pb.createVar("exp0", expand2);
    expansionCount[1] = pb.createVar("exp1", expand3);
    writeOutputStreamSet("LigatureCounts", expansionCount);
}

// =========================================================================
// STAGE 4: Transformation Kernel - Handles ALL Category 2 expansions
// =========================================================================
class Zawgyi2UnicodeTransformer : public pablo::PabloKernel {
public:
    Zawgyi2UnicodeTransformer(LLVMTypeSystemInterface & ts, StreamSet * ExpandedBasis, 
                             StreamSet * ZawgyiMask, StreamSet * ExpandedLigatureCounts, 
                             StreamSet * OutputU21);
protected:
    void generatePabloMethod() override;
};

Zawgyi2UnicodeTransformer::Zawgyi2UnicodeTransformer(LLVMTypeSystemInterface & ts, 
                                                     StreamSet * ExpandedBasis, 
                                                     StreamSet * ZawgyiMask,
                                                     StreamSet * ExpandedLigatureCounts, 
                                                     StreamSet * OutputU21)
: PabloKernel(ts, "Zawgyi2UnicodeTransformer", 
  {Binding{"ExpandedBasis", ExpandedBasis}, Binding{"ZawgyiMask", ZawgyiMask},
   Binding{"ExpandedLigatureCounts", ExpandedLigatureCounts}}, 
  {Binding{"OutputU21", OutputU21}}) {}

void Zawgyi2UnicodeTransformer::generatePabloMethod() {
    PabloBuilder pb(getEntryScope());
    std::vector<PabloAST *> basis = getInputStreamSet("ExpandedBasis");
    PabloAST * zawgyiMask = getInputStreamSet("ZawgyiMask")[0];
    std::vector<PabloAST *> ligatureCounts = getInputStreamSet("ExpandedLigatureCounts");
    BixNumCompiler bnc(pb);

    // ============================================================
    // PHASE 1: Simple 1-to-1 Mappings
    // ============================================================
    struct SimpleMapping {
        uint32_t zawgyi;
        uint32_t unicode;
    };
    std::vector<SimpleMapping> simpleMaps = {
        // Consonants
        {0x106A, 0x1009}, {0x106B, 0x100A}, {0x108F, 0x1014},
        {0x1090, 0x101B}, {0x1086, 0x103F},
        // Medials
        {0x103A, 0x103B}, {0x107D, 0x103B}, {0x107E, 0x103C},
        {0x107F, 0x103C}, {0x1080, 0x103C}, {0x1081, 0x103C},
        {0x1082, 0x103C}, {0x1083, 0x103C}, {0x1084, 0x103C},
        {0x108A, 0x103D}, {0x103D, 0x103E}, {0x1087, 0x103E},
        // Vowels
        {0x1033, 0x102F}, {0x1034, 0x1030},
        {0x1094, 0x1037}, {0x1095, 0x1037},
        // Kinzi vowels
        {0x108B, 0x102D}, {0x108C, 0x102E}, {0x108D, 0x1036}
    };

    std::vector<PabloAST *> outputBasis(21);
    for (unsigned i = 0; i < 21; ++i) {
        PabloAST * bit = basis[i];
        for (const auto &map : simpleMaps) {
            PabloAST * isMatch = bnc.EQ(basis, map.zawgyi);
            PabloAST * shouldTransform = pb.createAnd(zawgyiMask, isMatch);
            PabloAST * targetBit = createConstantBixNum(pb, map.unicode, 21)[i];
            bit = pb.createSel(shouldTransform, targetBit, bit);
        }
        outputBasis[i] = bit;
    }

    // ============================================================
    // PHASE 2: 1-to-Multiple Mappings (Category 2)
    // ============================================================
    
    // Define ALL stacked consonant mappings
    struct ExpansionMapping {
        uint32_t zawgyi;
        uint32_t unicode1;
        uint32_t unicode2;
        uint32_t unicode3;
    };
    
    std::vector<ExpansionMapping> expansions = {
        // Tall AA
        {0x105A, 0x102B, 0x103A, 0x0000},
        
        // Stacked consonants (2-char expansions)
        {0x1060, 0x1039, 0x1000, 0x0000},
        {0x1061, 0x1039, 0x1001, 0x0000},
        {0x1062, 0x1039, 0x1002, 0x0000},
        {0x1063, 0x1039, 0x1003, 0x0000},
        {0x1065, 0x1039, 0x1005, 0x0000},
        {0x1066, 0x1039, 0x1006, 0x0000},
        {0x1067, 0x1039, 0x1006, 0x0000},
        {0x1068, 0x1039, 0x1007, 0x0000},
        {0x1069, 0x1039, 0x1008, 0x0000},
        {0x106C, 0x1039, 0x100B, 0x0000},
        {0x106D, 0x1039, 0x100C, 0x0000},
        {0x1070, 0x1039, 0x100F, 0x0000},
        {0x1071, 0x1039, 0x1010, 0x0000},
        {0x1072, 0x1039, 0x1010, 0x0000},
        {0x1073, 0x1039, 0x1011, 0x0000},
        {0x1074, 0x1039, 0x1011, 0x0000},
        {0x1075, 0x1039, 0x1012, 0x0000},
        {0x1076, 0x1039, 0x1013, 0x0000},
        {0x1077, 0x1039, 0x1014, 0x0000},
        {0x1078, 0x1039, 0x1015, 0x0000},
        {0x1079, 0x1039, 0x1016, 0x0000},
        {0x107A, 0x1039, 0x1017, 0x0000},
        {0x107B, 0x1039, 0x1018, 0x0000},
        {0x107C, 0x1039, 0x1019, 0x0000},
        {0x1085, 0x1039, 0x101C, 0x0000},
        {0x1093, 0x1039, 0x1018, 0x0000},
        {0x1088, 0x103E, 0x102F, 0x0000},
        {0x1089, 0x103E, 0x1030, 0x0000},
        
        // 3-character expansions
        {0x1096, 0x1039, 0x1010, 0x103D}
    };

    // Apply each expansion
    for (const auto &exp : expansions) {
        PabloAST * isMatch = bnc.EQ(basis, exp.zawgyi);
        
        // First position: unicode1
        std::vector<PabloAST *> target1 = createConstantBixNum(pb, exp.unicode1, 21);
        for (unsigned i = 0; i < 21; ++i) {
            outputBasis[i] = pb.createSel(isMatch, target1[i], outputBasis[i]);
        }
        
        // Second position (inserted): unicode2
        if (exp.unicode2 != 0) {
            PabloAST * pos2 = pb.createAdvance(isMatch, 1);
            std::vector<PabloAST *> target2 = createConstantBixNum(pb, exp.unicode2, 21);
            for (unsigned i = 0; i < 21; ++i) {
                outputBasis[i] = pb.createSel(pos2, target2[i], outputBasis[i]);
            }
        }
        
        // Third position (inserted): unicode3
        if (exp.unicode3 != 0) {
            PabloAST * pos3 = pb.createAdvance(isMatch, 2);
            std::vector<PabloAST *> target3 = createConstantBixNum(pb, exp.unicode3, 21);
            for (unsigned i = 0; i < 21; ++i) {
                outputBasis[i] = pb.createSel(pos3, target3[i], outputBasis[i]);
            }
        }
    }

    writeOutputStreamSet("OutputU21", outputBasis);
}

typedef void (*XfrmFunctionType)(uint32_t fd);

XfrmFunctionType generate_pipeline(CPUDriver & driver) {
    auto P = CreatePipeline(driver, Input<uint32_t>("inputFileDescriptor"));

    // STAGE 1: Ingestion
    Scalar * fileDescriptor = P.getInputScalar("inputFileDescriptor");
    StreamSet * ByteStream = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<ReadSourceKernel>(fileDescriptor, ByteStream);

    StreamSet * BasisBits = P.CreateStreamSet(8, 1);
    P.CreateKernelCall<S2PKernel>(ByteStream, BasisBits);

    // STAGE 2: UTF-8 Decoding
    StreamSet * u8index = P.CreateStreamSet(1, 1);
    P.CreateKernelCall<UTF8_index>(BasisBits, u8index);

    StreamSet * U21_u8indexed = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<UTF8_Decoder>(BasisBits, U21_u8indexed);

    StreamSet * U21 = P.CreateStreamSet(21, 1);
    FilterByMask(P, u8index, U21_u8indexed, U21);

    // STAGE 3: Classification
    StreamSet * ZawgyiMask = P.CreateStreamSet(1, 1);
    StreamSet * LigatureCounts = P.CreateStreamSet(2, 1);
    P.CreateKernelCall<ZawgyiClassifier>(U21, ZawgyiMask, LigatureCounts);

    // STAGE 3.5: Expansion using Spread Masks
    StreamSet * SpreadMask = P.CreateStreamSet(1, 1);
    InsertionSpreadMask(P, LigatureCounts, SpreadMask, kernel::InsertPosition::After);

    StreamSet * ExpandedBasis = P.CreateStreamSet(21, 1);
    SpreadByMask(P, SpreadMask, U21, ExpandedBasis);

    StreamSet * ExpandedZawgyiMask = P.CreateStreamSet(1, 1);
    SpreadByMask(P, SpreadMask, ZawgyiMask, ExpandedZawgyiMask);

    StreamSet * ExpandedLigatureCounts = P.CreateStreamSet(2, 1);
    SpreadByMask(P, SpreadMask, LigatureCounts, ExpandedLigatureCounts);

    // STAGE 4: Transformation
    StreamSet * TransformedU21 = P.CreateStreamSet(21, 1);
    P.CreateKernelCall<Zawgyi2UnicodeTransformer>(ExpandedBasis, ExpandedZawgyiMask, 
                                                 ExpandedLigatureCounts, TransformedU21);

    // STAGE 5 & 6: Output
    StreamSet * const OutputBasis = P.CreateStreamSet(8);
    U21_to_UTF8(P, TransformedU21, OutputBasis);

    StreamSet * OutputBytes = P.CreateStreamSet(1, 8);
    P.CreateKernelCall<P2SKernel>(OutputBasis, OutputBytes);
    P.CreateKernelCall<StdOutKernel>(OutputBytes);

    return P.compile();
}

int main(int argc, char *argv[]) {
    codegen::ParseCommandLineOptions(argc, argv, {&ZawgyiOptions, &codegen::JIT_InfoOptions});
    CPUDriver driver("zawgyi2unicode");
    XfrmFunctionType fn = generate_pipeline(driver);
    const int fd = open(inputFile.c_str(), O_RDONLY);
    if (LLVM_UNLIKELY(fd == -1)) {
        llvm::errs() << "Error: cannot open " << inputFile << " for processing.\n";
    } else {
        fn(fd);
        close(fd);
    }
    return 0;
}