/*
 * Modification Copyright 2025 ByteDance Ltd. and/or its affiliates.
 */
#include "triton/Dialect/TritonGPU/Transforms/PipeliningUtility.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Support/LLVM.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/Transforms/Passes.h"
#include "triton/Dialect/TritonGPU/Transforms/Utility.h"
#include "triton/Dialect/TritonNvidiaGPU/IR/Dialect.h"
#include "triton/Dialect/TritonNvidiaGPU/Transforms/TMAUtilities.h"
#include "llvm/Support/Casting.h"

#include "TritonDistributed/Dialect/Distributed/IR/Dialect.h"

using namespace mlir;
namespace tt = mlir::triton;
namespace ttg = mlir::triton::gpu;
namespace ttng = mlir::triton::nvidia_gpu;

bool mlir::triton::loopHasDistGreaterThanOne(scf::ForOp forOp) {
  return llvm::any_of(forOp.getBody()->getTerminator()->getOperands(),
                      [](Value operand) {
                        Operation *def = operand.getDefiningOp();
                        return !def;
                      });
}

bool mlir::triton::isOuterLoop(scf::ForOp forOp) {
  return llvm::any_of(forOp.getBody()->getOperations(), [](Operation &op) {
    return isa<scf::ForOp, scf::WhileOp>(op);
  });
}

// Combine the current mask with the given predicate.
Value mlir::triton::getPredMask(RewriterBase &rewriter, Type typeLike,
                                Value currentMask, Value pred) {
  Type maskType = tt::getI1SameShape(typeLike);
  Location loc = pred.getLoc();
  Value mask = pred;
  if (isa<RankedTensorType>(maskType)) {
    mask = rewriter.create<tt::SplatOp>(loc, maskType, pred);
  }
  if (currentMask) {
    mask = rewriter.create<arith::AndIOp>(loc, mask, currentMask);
  }
  return mask;
}

// Function to mask operations during scheduling.
Operation *mlir::triton::predicateOp(RewriterBase &rewriter, Operation *op,
                                     Value pred) {
  OpBuilder::InsertionGuard guard(rewriter);
  if (mlir::isMemoryEffectFree(op))
    return op;
  if (isa<LLVM::AssumeOp>(op))
    return op;
  if (isa<ttg::AsyncCommitGroupOp, ttg::AsyncWaitOp>(op))
    return op;
  if (isa<ttg::LocalLoadOp, ttg::LocalStoreOp>(op))
    return op;
  if (isa<ttng::TMEMAllocOp, ttng::TMEMLoadOp>(op))
    return op;
  if (auto ifOp = dyn_cast<scf::IfOp>(op)) {
    rewriter.setInsertionPoint(op);
    Value cnd = getPredMask(rewriter, ifOp.getCondition().getType(),
                            ifOp.getCondition(), pred);
    ifOp.getConditionMutable().assign(cnd);
    return op;
  }
  if (auto asyncCopyOp = dyn_cast<ttg::AsyncCopyGlobalToLocalOp>(op)) {
    rewriter.setInsertionPoint(asyncCopyOp);
    Value mask = getPredMask(rewriter, asyncCopyOp.getSrc().getType(),
                             asyncCopyOp.getMask(), pred);
    asyncCopyOp.getMaskMutable().assign(mask);
    return op;
  }
  if (auto loadOp = dyn_cast<tt::LoadOp>(op)) {
    rewriter.setInsertionPoint(loadOp);
    Value mask = getPredMask(rewriter, loadOp.getPtr().getType(),
                             loadOp.getMask(), pred);
    loadOp.getMaskMutable().assign(mask);
    return op;
  }
  if (auto copyOp = dyn_cast<ttng::AsyncTMACopyGlobalToLocalOp>(op)) {
    rewriter.setInsertionPoint(copyOp);
    Value mask = getPredMask(rewriter, copyOp.getPred().getType(),
                             copyOp.getPred(), pred);
    copyOp.getPredMutable().assign(mask);
    return op;
  }
  if (auto gatherOp = dyn_cast<ttng::AsyncTMAGatherOp>(op)) {
    rewriter.setInsertionPoint(gatherOp);
    Value mask = getPredMask(rewriter, gatherOp.getPred().getType(),
                             gatherOp.getPred(), pred);
    gatherOp.getPredMutable().assign(mask);
    return op;
  }
  if (auto expectOp = dyn_cast<ttng::BarrierExpectOp>(op)) {
    rewriter.setInsertionPoint(expectOp);
    Value mask = getPredMask(rewriter, expectOp.getPred().getType(),
                             expectOp.getPred(), pred);
    expectOp.getPredMutable().assign(mask);
    return op;
  }
  if (auto mmav5Op = dyn_cast<ttng::MMAv5OpInterface>(op)) {
    rewriter.setInsertionPoint(mmav5Op);
    auto currPred = mmav5Op.getPredicate();
    Value mask = getPredMask(rewriter, currPred.getType(), currPred, pred);
    mmav5Op.setPredicate(mask);
    return op;
  }
  if (auto tmemStoreOp = dyn_cast<ttng::TMEMStoreOp>(op)) {
    rewriter.setInsertionPoint(tmemStoreOp);
    Value mask = getPredMask(rewriter, tmemStoreOp.getPred().getType(),
                             tmemStoreOp.getPred(), pred);
    tmemStoreOp.getPredMutable().assign(mask);
    return op;
  }
  if (auto waitBarrier = dyn_cast<ttng::WaitBarrierOp>(op)) {
    rewriter.setInsertionPoint(waitBarrier);
    Value mask = pred;
    Value currentPred = waitBarrier.getPred();
    if (currentPred) {
      mask = getPredMask(rewriter, currentPred.getType(), currentPred, pred);
    }
    waitBarrier.getPredMutable().assign(mask);
    return op;
  }
  if (auto arriveBarrier = dyn_cast<ttng::ArriveBarrierOp>(op)) {
    rewriter.setInsertionPoint(arriveBarrier);
    Value mask = pred;
    Value currentPred = arriveBarrier.getPred();
    if (currentPred) {
      mask = getPredMask(rewriter, currentPred.getType(), currentPred, pred);
    }
    arriveBarrier.getPredMutable().assign(mask);
    return op;
  }
  if (auto storeOp = dyn_cast<tt::StoreOp>(op)) {
    rewriter.setInsertionPoint(storeOp);
    Value mask = getPredMask(rewriter, storeOp.getPtr().getType(),
                             storeOp.getMask(), pred);
    storeOp.getMaskMutable().assign(mask);
    return op;
  }
  if (auto atomicRMWOp = dyn_cast<tt::AtomicRMWOp>(op)) {
    rewriter.setInsertionPoint(atomicRMWOp);
    Value mask = getPredMask(rewriter, atomicRMWOp.getPtr().getType(),
                             atomicRMWOp.getMask(), pred);
    atomicRMWOp.getMaskMutable().assign(mask);
    return op;
  }
  // TODO(zhengsize): add predicate to distributed ops?
  // Distributed barrier ops
  if (isa<triton::distributed::ConsumeTokenOp>(op))
    return op;
  if (isa<triton::distributed::WaitOp>(op)) {
    // fallback to branch
    scf::IfOp newIfOp = rewriter.create<scf::IfOp>(
        op->getLoc(), op->getResultTypes(), pred, true);
    auto thenB = newIfOp.getThenBodyBuilder();
    auto newOpInThen = thenB.clone(*op);
    thenB.create<scf::YieldOp>(op->getLoc(), newOpInThen->getResults());
    auto elseB = newIfOp.getElseBodyBuilder();
    auto elseConst = elseB.create<arith::ConstantOp>(
        op->getLoc(), IntegerAttr::get(op->getResultTypes().front(), 0));
    elseB.create<scf::YieldOp>(op->getLoc(), elseConst->getResults());
    rewriter.replaceOp(op, newIfOp.getResults());
    return newIfOp;
  }

  op->emitError("pipeliner doesn't know how to predicate this op.");
  llvm::report_fatal_error("Fatal pipeliner error");
  return op;
}

void mlir::triton::replaceUsesAndPropagateType(OpBuilder &builder,
                                               Operation *oldUse, Value val) {
  SmallVector<Operation *> opsToDelete;
  SmallVector<OpOperand *> operandsToReplace;

  // Save the operand to replace / delete later (avoid iterator invalidation).
  // TODO: can we use an early_inc iterator?
  for (OpOperand &use : oldUse->getUses()) {
    // Non-subview/trans ops will be replaced by `val`.
    if (!isa<triton::gpu::MemDescTransOp, triton::gpu::MemDescSubviewOp>(
            use.getOwner())) {
      operandsToReplace.push_back(&use);
      continue;
    }
    Operation *user = use.getOwner();
    // `subview(old_op)` is replaced by a new `subview(val)`.
    OpBuilder::InsertionGuard g(builder);
    builder.setInsertionPoint(user);
    Value newVal;
    if (auto subview = dyn_cast<triton::gpu::MemDescSubviewOp>(user)) {
      triton::gpu::MemDescType oldType = subview.getType();
      bool isMutable =
          cast<triton::gpu::MemDescType>(val.getType()).getMutableMemory();
      Type newDstType = triton::gpu::MemDescType::get(
          oldType.getShape(), oldType.getElementType(), oldType.getEncoding(),
          oldType.getMemorySpace(), isMutable);
      newVal = builder.create<triton::gpu::MemDescSubviewOp>(
          subview.getLoc(), newDstType, val, subview.getOffsets());
      newVal.getDefiningOp()->setAttrs(user->getAttrs());
    } else if (auto trans = dyn_cast<triton::gpu::MemDescTransOp>(user)) {
      newVal = builder.create<triton::gpu::MemDescTransOp>(trans.getLoc(), val,
                                                           trans.getOrder());
      newVal.getDefiningOp()->setAttrs(user->getAttrs());
    }
    assert(newVal);
    replaceUsesAndPropagateType(builder, user, newVal);
    opsToDelete.push_back(use.getOwner());
  }

  // Perform late replacement.
  for (OpOperand *operand : operandsToReplace) {
    Operation *op = operand->getOwner();
    operand->set(val);
  }

  // Perform late op erasure.
  for (Operation *op : opsToDelete)
    op->erase();
}

// Return true if the given ForOp has the attribute
// `tt.disallow_acc_multi_buffer` set to true.
bool mlir::triton::getDisallowAccMultiBuffer(scf::ForOp forOp) {
  return forOp->hasAttr(mlir::triton::kDisallowAccMultiBufferAttrName);
}

std::pair<OpResult, int64_t>
mlir::triton::getDefinitionAndDistance(scf::ForOp forOp, Value value) {
  int64_t distance = 0;
  while (auto arg = dyn_cast<BlockArgument>(value)) {
    // Ignore implicit captures.
    if (arg.getOwner() != forOp.getBody())
      return {nullptr, 0};
    // Ignore induction variable.
    if (arg.getArgNumber() == 0)
      return {nullptr, 0};
    ++distance;
    value = forOp.getYieldedValues()[arg.getArgNumber() - 1];
  }
  return {cast<OpResult>(value), distance};
}

std::pair<Operation *, int64_t>
mlir::triton::getDefiningOpAndDistance(scf::ForOp forOp, Value value) {
  auto [definition, distance] = getDefinitionAndDistance(forOp, value);
  return {definition ? definition.getDefiningOp() : nullptr, distance};
}

int mlir::triton::getCopyVecBytes(RankedTensorType registerTy,
                                  ttg::SharedEncodingTrait sharedEnc) {
  auto regLayout = triton::gpu::toLinearLayout(registerTy.getShape(),
                                               registerTy.getEncoding());
  auto sharedLayout =
      triton::gpu::toLinearLayout(registerTy.getShape(), sharedEnc);
  auto regToSharedLayout = regLayout.invertAndCompose(sharedLayout);
  const int vecElems = regToSharedLayout.getNumConsecutiveInOut();
  return vecElems * registerTy.getElementTypeBitWidth() / 8;
}

void mlir::triton::serializeLatencies(ModuleOp module,
                                      DenseMap<Operation *, int> &opLatency) {
  for (auto &[op, latency] : opLatency) {
    op->setAttr(
        kLatencyAttrName,
        IntegerAttr::get(IntegerType::get(module.getContext(), 32), latency));
  }
}

DenseMap<Operation *, int> mlir::triton::deserializeLatencies(Operation *op) {
  DenseMap<Operation *, int> opLatency;
  op->walk([&](Operation *op) {
    if (op->hasAttr(kLatencyAttrName)) {
      opLatency[op] = op->getAttrOfType<IntegerAttr>(kLatencyAttrName).getInt();
      op->removeAttr(kLatencyAttrName);
    }
  });
  return opLatency;
}

Value mlir::triton::createScalarAlloc(ImplicitLocOpBuilder &rewriter, Type type,
                                      unsigned numBuffers) {
  MLIRContext *ctx = rewriter.getContext();
  unsigned numCTAs = triton::gpu::TritonGPUDialect::getNumCTAs(
      rewriter.getBlock()->getParentOp()->getParentOfType<ModuleOp>());
  Attribute sharedMemorySpace =
      ttg::SharedMemorySpaceAttr::get(rewriter.getContext());
  auto barrierCTALayout =
      ttg::CTALayoutAttr::get(/*context=*/ctx, /*CTAsPerCGA=*/{numCTAs},
                              /*CTASplitNum=*/{1}, /*CTAOrder=*/{0});
  auto barrierEncoding =
      ttg::SwizzledSharedEncodingAttr::get(ctx, 1, 1, 1, {0}, barrierCTALayout);
  ttg::MemDescType memDescType = ttg::MemDescType::get(
      {numBuffers}, type, barrierEncoding, sharedMemorySpace,
      /*mutableMemory=*/true);
  return rewriter.create<ttg::LocalAllocOp>(memDescType, Value());
}

// Create an allocation and init the mbarriers.
Value mlir::triton::createBarrierAlloc(scf::ForOp forOp, int numBarriers) {
  ImplicitLocOpBuilder rewriter(forOp.getLoc(), forOp);

  Value barrierAlloc =
      createScalarAlloc(rewriter, rewriter.getI64Type(), numBarriers);
  for (unsigned i = 0; i < numBarriers; i++) {
    Value barrierView = createSingleBufferView(rewriter, barrierAlloc, i);
    rewriter.create<ttng::InitBarrierOp>(barrierView, 1);
  }
  // Invalidate and deallocate the barriers.
  rewriter.setInsertionPointAfter(forOp);
  for (unsigned i = 0; i < numBarriers; i++) {
    Value barrierView = createSingleBufferView(rewriter, barrierAlloc, i);
    rewriter.create<ttng::InvalBarrierOp>(barrierView);
  }
  rewriter.create<ttg::LocalDeallocOp>(barrierAlloc);
  return barrierAlloc;
}

Value mlir::triton::createAlloc(scf::ForOp forOp, RankedTensorType ty,
                                Location loc,
                                gpu::SharedEncodingTrait sharedEnc,
                                unsigned distance) {
  OpBuilder builder(forOp);
  Attribute sharedMemorySpace =
      ttg::SharedMemorySpaceAttr::get(forOp.getContext());
  SmallVector<int64_t> bufferShape(ty.getShape().begin(), ty.getShape().end());
  bufferShape.insert(bufferShape.begin(), distance);
  Type memdescType = ttg::MemDescType::get(bufferShape, ty.getElementType(),
                                           sharedEnc, sharedMemorySpace,
                                           /*mutableMemory=*/true);
  Value alloc = builder.create<ttg::LocalAllocOp>(loc, memdescType);

  builder.setInsertionPointAfter(forOp);
  builder.create<ttg::LocalDeallocOp>(forOp.getLoc(), alloc);
  return alloc;
}

bool mlir::triton::isTMALoad(Operation *op) {
  return isa<tt::DescriptorLoadOp, tt::DescriptorGatherOp>(op);
}

ttg::MemDescType mlir::triton::getBufferViewType(ttg::MemDescType allocTy) {
  Attribute sharedMemorySpace =
      ttg::SharedMemorySpaceAttr::get(allocTy.getContext());
  return ttg::MemDescType::get(allocTy.getShape().drop_front(),
                               allocTy.getElementType(), allocTy.getEncoding(),
                               sharedMemorySpace, /*mutableMemory=*/true,
                               /*allocShape=*/allocTy.getAllocShape());
}

ttg::SharedEncodingTrait mlir::triton::getSharedEncoding(RankedTensorType ty) {
  auto ctaLayout = ttg::getCTALayout(ty.getEncoding());
  auto order = ttg::getOrder(ty);
  // Use generic layout. This won't be optimal for 2D tensors.
  return ttg::SwizzledSharedEncodingAttr::get(ty.getContext(), 1, 1, 1, order,
                                              ctaLayout);
}

ttg::SharedEncodingTrait mlir::triton::getSharedEncoding(Operation *op) {
  // Try to use local alloc encoding if possible.
  ttg::SharedEncodingTrait localAllocEnc;
  if (llvm::any_of(op->getUsers(), [&](Operation *user) {
        return isa<ttg::LocalAllocOp>(user);
      })) {
    for (auto user : op->getUsers()) {
      auto localAlloc = dyn_cast<ttg::LocalAllocOp>(user);
      if (!localAlloc)
        continue;
      auto enc = mlir::cast<ttg::SharedEncodingTrait>(
          localAlloc.getType().getEncoding());
      if (!localAllocEnc) {
        localAllocEnc = enc;
      }
      if (enc != localAllocEnc) {
        // Some users have different encoding than others.
        // Use one of the encodings, and warn about the performance issue.
        op->emitRemark()
            << "Pipelining load with different use encodings. This will lead "
               "to layout conversions and performance degradation.";
        continue;
      }
    }
  }

  auto ty = cast<RankedTensorType>(op->getResultTypes()[0]);
  auto ctaLayout = ttg::getCTALayout(ty.getEncoding());
  auto order = ttg::getOrder(ty);
  if (isTMALoad(op)) {
    // TMA encoding is set on the descriptor type
    TypedValue<tt::TensorDescType> desc;
    if (auto load = dyn_cast<tt::DescriptorLoadOp>(op)) {
      desc = load.getDesc();
    } else if (auto gather = dyn_cast<tt::DescriptorGatherOp>(op)) {
      desc = gather.getDesc();
    } else {
      op->emitError() << "unrecognized tma load type";
      llvm::report_fatal_error("unrecognized tma load type");
    }
    return ttng::getEncodingFromDescriptor(op, ty, desc);
  }

  if (localAllocEnc)
    return localAllocEnc;

  // Try to use dot encoding if possible.
  bool incompatible = false;
  localAllocEnc =
      getSharedEncIfAllUsersAreDotEnc(op->getResult(0), incompatible)
          .value_or(nullptr);

  if (localAllocEnc)
    return localAllocEnc;

  // Use generic layout. This won't be optimal for 2D tensors.
  return ttg::SwizzledSharedEncodingAttr::get(ty.getContext(), 1, 1, 1, order,
                                              ctaLayout);
}

int mlir::triton::getNumStagesOrDefault(scf::ForOp forOp,
                                        int defaultNumStages) {
  // Use the attribute attached to the loop if it exists otherwise use the
  // global control.
  if (!forOp->hasAttr(mlir::triton::kNumStagesAttrName))
    return defaultNumStages;
  return mlir::cast<IntegerAttr>(
             forOp->getAttr(mlir::triton::kNumStagesAttrName))
      .getInt();
}
