//===- InstSimplifyFolder.h - InstSimplify folding helper --------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the InstSimplifyFolder class, a helper for IRBuilder.
// It provides IRBuilder with a set of methods for folding operations to
// existing values using InstructionSimplify. At the moment, only a subset of
// the implementation uses InstructionSimplify. The rest of the implementation
// only folds constants.
//
// The folder also applies target-specific constant folding.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_INSTSIMPLIFYFOLDER_H
#define LLVM_ANALYSIS_INSTSIMPLIFYFOLDER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/TargetFolder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilderFolder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"

namespace llvm {

/// InstSimplifyFolder - Use InstructionSimplify to fold operations to existing
/// values. Also applies target-specific constant folding when not using
/// InstructionSimplify.
class InstSimplifyFolder final : public IRBuilderFolder {
  TargetFolder ConstFolder;
  SimplifyQuery SQ;

  virtual void anchor();

public:
  InstSimplifyFolder(const DataLayout &DL) : ConstFolder(DL), SQ(DL) {}

  //===--------------------------------------------------------------------===//
  // Value-based folders.
  //
  // Return an existing value or a constant if the operation can be simplified.
  // Otherwise return nullptr.
  //===--------------------------------------------------------------------===//
  Value *FoldAdd(Value *LHS, Value *RHS, bool HasNUW = false,
                 bool HasNSW = false) const override {
    return SimplifyAddInst(LHS, RHS, HasNUW, HasNSW, SQ);
  }

  Value *FoldOr(Value *LHS, Value *RHS) const override {
    return SimplifyOrInst(LHS, RHS, SQ);
  }

  //===--------------------------------------------------------------------===//
  // Binary Operators
  //===--------------------------------------------------------------------===//

  Value *CreateFAdd(Constant *LHS, Constant *RHS) const override {
    return ConstFolder.CreateFAdd(LHS, RHS);
  }
  Value *CreateSub(Constant *LHS, Constant *RHS, bool HasNUW = false,
                   bool HasNSW = false) const override {
    return ConstFolder.CreateSub(LHS, RHS, HasNUW, HasNSW);
  }
  Value *CreateFSub(Constant *LHS, Constant *RHS) const override {
    return ConstFolder.CreateFSub(LHS, RHS);
  }
  Value *CreateMul(Constant *LHS, Constant *RHS, bool HasNUW = false,
                   bool HasNSW = false) const override {
    return ConstFolder.CreateMul(LHS, RHS, HasNUW, HasNSW);
  }
  Value *CreateFMul(Constant *LHS, Constant *RHS) const override {
    return ConstFolder.CreateFMul(LHS, RHS);
  }
  Value *CreateUDiv(Constant *LHS, Constant *RHS,
                    bool isExact = false) const override {
    return ConstFolder.CreateUDiv(LHS, RHS, isExact);
  }
  Value *CreateSDiv(Constant *LHS, Constant *RHS,
                    bool isExact = false) const override {
    return ConstFolder.CreateSDiv(LHS, RHS, isExact);
  }
  Value *CreateFDiv(Constant *LHS, Constant *RHS) const override {
    return ConstFolder.CreateFDiv(LHS, RHS);
  }
  Value *CreateURem(Constant *LHS, Constant *RHS) const override {
    return ConstFolder.CreateURem(LHS, RHS);
  }
  Value *CreateSRem(Constant *LHS, Constant *RHS) const override {
    return ConstFolder.CreateSRem(LHS, RHS);
  }
  Value *CreateFRem(Constant *LHS, Constant *RHS) const override {
    return ConstFolder.CreateFRem(LHS, RHS);
  }
  Value *CreateShl(Constant *LHS, Constant *RHS, bool HasNUW = false,
                   bool HasNSW = false) const override {
    return ConstFolder.CreateShl(LHS, RHS, HasNUW, HasNSW);
  }
  Value *CreateLShr(Constant *LHS, Constant *RHS,
                    bool isExact = false) const override {
    return ConstFolder.CreateLShr(LHS, RHS, isExact);
  }
  Value *CreateAShr(Constant *LHS, Constant *RHS,
                    bool isExact = false) const override {
    return ConstFolder.CreateAShr(LHS, RHS, isExact);
  }
  Value *CreateAnd(Constant *LHS, Constant *RHS) const override {
    return ConstFolder.CreateAnd(LHS, RHS);
  }
  Value *CreateXor(Constant *LHS, Constant *RHS) const override {
    return ConstFolder.CreateXor(LHS, RHS);
  }

  Value *CreateBinOp(Instruction::BinaryOps Opc, Constant *LHS,
                     Constant *RHS) const override {
    return ConstFolder.CreateBinOp(Opc, LHS, RHS);
  }

  //===--------------------------------------------------------------------===//
  // Unary Operators
  //===--------------------------------------------------------------------===//

  Value *CreateNeg(Constant *C, bool HasNUW = false,
                   bool HasNSW = false) const override {
    return ConstFolder.CreateNeg(C, HasNUW, HasNSW);
  }
  Value *CreateFNeg(Constant *C) const override {
    return ConstFolder.CreateFNeg(C);
  }
  Value *CreateNot(Constant *C) const override {
    return ConstFolder.CreateNot(C);
  }

  Value *CreateUnOp(Instruction::UnaryOps Opc, Constant *C) const override {
    return ConstFolder.CreateUnOp(Opc, C);
  }

  //===--------------------------------------------------------------------===//
  // Memory Instructions
  //===--------------------------------------------------------------------===//

  Value *CreateGetElementPtr(Type *Ty, Constant *C,
                             ArrayRef<Constant *> IdxList) const override {
    return ConstFolder.CreateGetElementPtr(Ty, C, IdxList);
  }
  Value *CreateGetElementPtr(Type *Ty, Constant *C,
                             Constant *Idx) const override {
    // This form of the function only exists to avoid ambiguous overload
    // warnings about whether to convert Idx to ArrayRef<Constant *> or
    // ArrayRef<Value *>.
    return ConstFolder.CreateGetElementPtr(Ty, C, Idx);
  }
  Value *CreateGetElementPtr(Type *Ty, Constant *C,
                             ArrayRef<Value *> IdxList) const override {
    return ConstFolder.CreateGetElementPtr(Ty, C, IdxList);
  }

  Value *
  CreateInBoundsGetElementPtr(Type *Ty, Constant *C,
                              ArrayRef<Constant *> IdxList) const override {
    return ConstFolder.CreateInBoundsGetElementPtr(Ty, C, IdxList);
  }
  Value *CreateInBoundsGetElementPtr(Type *Ty, Constant *C,
                                     Constant *Idx) const override {
    // This form of the function only exists to avoid ambiguous overload
    // warnings about whether to convert Idx to ArrayRef<Constant *> or
    // ArrayRef<Value *>.
    return ConstFolder.CreateInBoundsGetElementPtr(Ty, C, Idx);
  }
  Value *CreateInBoundsGetElementPtr(Type *Ty, Constant *C,
                                     ArrayRef<Value *> IdxList) const override {
    return ConstFolder.CreateInBoundsGetElementPtr(Ty, C, IdxList);
  }

  //===--------------------------------------------------------------------===//
  // Cast/Conversion Operators
  //===--------------------------------------------------------------------===//

  Value *CreateCast(Instruction::CastOps Op, Constant *C,
                    Type *DestTy) const override {
    if (C->getType() == DestTy)
      return C; // avoid calling Fold
    return ConstFolder.CreateCast(Op, C, DestTy);
  }
  Value *CreateIntCast(Constant *C, Type *DestTy,
                       bool isSigned) const override {
    if (C->getType() == DestTy)
      return C; // avoid calling Fold
    return ConstFolder.CreateIntCast(C, DestTy, isSigned);
  }
  Value *CreatePointerCast(Constant *C, Type *DestTy) const override {
    if (C->getType() == DestTy)
      return C; // avoid calling Fold
    return ConstFolder.CreatePointerCast(C, DestTy);
  }
  Value *CreateFPCast(Constant *C, Type *DestTy) const override {
    if (C->getType() == DestTy)
      return C; // avoid calling Fold
    return ConstFolder.CreateFPCast(C, DestTy);
  }
  Value *CreateBitCast(Constant *C, Type *DestTy) const override {
    return ConstFolder.CreateBitCast(C, DestTy);
  }
  Value *CreateIntToPtr(Constant *C, Type *DestTy) const override {
    return ConstFolder.CreateIntToPtr(C, DestTy);
  }
  Value *CreatePtrToInt(Constant *C, Type *DestTy) const override {
    return ConstFolder.CreatePtrToInt(C, DestTy);
  }
  Value *CreateZExtOrBitCast(Constant *C, Type *DestTy) const override {
    if (C->getType() == DestTy)
      return C; // avoid calling Fold
    return ConstFolder.CreateZExtOrBitCast(C, DestTy);
  }
  Value *CreateSExtOrBitCast(Constant *C, Type *DestTy) const override {
    if (C->getType() == DestTy)
      return C; // avoid calling Fold
    return ConstFolder.CreateSExtOrBitCast(C, DestTy);
  }
  Value *CreateTruncOrBitCast(Constant *C, Type *DestTy) const override {
    if (C->getType() == DestTy)
      return C; // avoid calling Fold
    return ConstFolder.CreateTruncOrBitCast(C, DestTy);
  }

  Value *CreatePointerBitCastOrAddrSpaceCast(Constant *C,
                                             Type *DestTy) const override {
    if (C->getType() == DestTy)
      return C; // avoid calling Fold
    return ConstFolder.CreatePointerBitCastOrAddrSpaceCast(C, DestTy);
  }

  //===--------------------------------------------------------------------===//
  // Compare Instructions
  //===--------------------------------------------------------------------===//

  Value *CreateICmp(CmpInst::Predicate P, Constant *LHS,
                    Constant *RHS) const override {
    return ConstFolder.CreateICmp(P, LHS, RHS);
  }
  Value *CreateFCmp(CmpInst::Predicate P, Constant *LHS,
                    Constant *RHS) const override {
    return ConstFolder.CreateFCmp(P, LHS, RHS);
  }

  //===--------------------------------------------------------------------===//
  // Other Instructions
  //===--------------------------------------------------------------------===//

  Value *CreateSelect(Constant *C, Constant *True,
                      Constant *False) const override {
    return ConstFolder.CreateSelect(C, True, False);
  }

  Value *CreateExtractElement(Constant *Vec, Constant *Idx) const override {
    return ConstFolder.CreateExtractElement(Vec, Idx);
  }

  Value *CreateInsertElement(Constant *Vec, Constant *NewElt,
                             Constant *Idx) const override {
    return ConstFolder.CreateInsertElement(Vec, NewElt, Idx);
  }

  Value *CreateShuffleVector(Constant *V1, Constant *V2,
                             ArrayRef<int> Mask) const override {
    return ConstFolder.CreateShuffleVector(V1, V2, Mask);
  }

  Value *CreateExtractValue(Constant *Agg,
                            ArrayRef<unsigned> IdxList) const override {
    return ConstFolder.CreateExtractValue(Agg, IdxList);
  }

  Value *CreateInsertValue(Constant *Agg, Constant *Val,
                           ArrayRef<unsigned> IdxList) const override {
    return ConstFolder.CreateInsertValue(Agg, Val, IdxList);
  }
};

} // end namespace llvm

#endif // LLVM_ANALYSIS_INSTSIMPLIFYFOLDER_H
