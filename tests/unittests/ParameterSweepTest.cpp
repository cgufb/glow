/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "BackendTestUtils.h"

#include "glow/ExecutionEngine/ExecutionEngine.h"
#include "glow/Graph/Graph.h"
#include "glow/Graph/PlaceholderBindings.h"
#include "glow/Support/Random.h"

#include "gtest/gtest.h"

#include "llvm/ADT/STLExtras.h"

#include <functional>

using namespace glow;
using llvm::cast;

/// This matches the signature that is used for the parameterized tests here,
/// i.e. those passing three parameters via a single ::testing::Combine() into
/// INSTANTIATE_TEST_CASE_P_FOR_BACKEND_COMBINED_TEST().
using ThreeIntTupleConfig = std::tuple<std::string, std::tuple<int, int, int>>;

#define SET_BACKEND_KIND_AND_THREE_INT_PARAMS(CONFIG, BACKEND_NAME, PARAM1,    \
                                              PARAM2, PARAM3)                  \
  std::tuple<int, int, int> threeIntTupleParams;                               \
  std::tie(BACKEND_NAME, threeIntTupleParams) = CONFIG;                        \
  std::tie(PARAM1, PARAM2, PARAM3) = threeIntTupleParams;

//===--------------------------------------------------------------------===//
//                   Convolution Parameter Sweep Tests
//===--------------------------------------------------------------------===//

/// Create a simple network that has a single fp convolution.
static FunctionTensorPair
createAndInitConvNet(glow::PlaceholderBindings &bindings,
                     glow::ExecutionEngine &EE, size_t size, size_t convDepth,
                     size_t kernel, size_t stride, size_t pad) {
  PseudoRNG PRNG;
  auto &mod = EE.getModule();
  Function *F = mod.createFunction("main");
  auto *var = mod.createPlaceholder(ElemKind::FloatTy,
                                    {1, size, size, convDepth}, "var", false);
  bindings.allocate(var)->getHandle().initXavier(1, PRNG);

  auto *conv =
      F->createConv(bindings, "conv", var, convDepth, kernel, stride, pad, 1);
  bindings.get(cast<Placeholder>(conv->getFilter()))->getHandle().clear(0.1);
  bindings.get(cast<Placeholder>(conv->getBias()))->getHandle().clear(0.1);
  auto *result = F->createSave("ret", conv);
  auto *resultTensor = bindings.allocate(result->getPlaceholder());
  convertPlaceholdersToConstants(F, bindings, {var, result->getPlaceholder()});

  return std::make_pair(F, resultTensor);
}

/// Helper to test sweeping across a variety of configurations of a convolution
/// by comparing the results to the Interpreter given some \p allowedError.
/// \p config contains the backend to compare the Interpreter against, plus the
/// specific configuration to run for this test. \p interpK and \p backendK are
/// the element kinds to use for the Interpreter and backend, respectively.
static void testParamSweepConv(ThreeIntTupleConfig config, ElemKind interpK,
                               ElemKind backendK, float allowedError) {
  std::string backend;
  size_t size, depth, kernel;
  SET_BACKEND_KIND_AND_THREE_INT_PARAMS(config, backend, size, depth, kernel)

  LOG(INFO) << "Testing Conv with size: " << size << "; depth: " << depth
            << "; kernel: " << kernel << "\n";

  auto boundF = std::bind(createAndInitConvNet, std::placeholders::_1,
                          std::placeholders::_2, size, depth, kernel,
                          /* stride */ 1, /* pad */ 0);
  compareAgainstInterpreter(backend, boundF, interpK, backendK, allowedError,
                            parCloneCountOpt);
}

DECLARE_STATELESS_BACKEND_TEST(ConvSweepTest, ThreeIntTupleConfig);

INSTANTIATE_TEST_CASE_P_FOR_BACKEND_COMBINED_TEST(
    SweepTest, ConvSweepTest,
    ::testing::Combine(/* size */ ::testing::Values(5, 7, 15),
                       /* depth */ ::testing::Values(8, 64),
                       /* kernel */ ::testing::Values(1, 3)));

/// Compare backend against the interpreter in Float.
TEST_P(ConvSweepTest, ConvTest_Float) {
  ENABLED_BACKENDS(CPU, OpenCL);
  testParamSweepConv(GetParam(), ElemKind::FloatTy, ElemKind::FloatTy, 0.0001f);
}

/// Compare backend against the interpreter in Int8.
TEST_P(ConvSweepTest, ConvTest_Int8) {
  ENABLED_BACKENDS(Interpreter, CPU, OpenCL);
  testParamSweepConv(GetParam(), ElemKind::FloatTy, ElemKind::Int8QTy, 0.045f);
}

/// Compare backend against the interpreter in FP16.
TEST_P(ConvSweepTest, ConvTest_Float16) {
  ENABLED_BACKENDS(Interpreter);
  testParamSweepConv(GetParam(), ElemKind::FloatTy, ElemKind::Float16Ty,
                     0.005f);
}

//===--------------------------------------------------------------------===//
//                   BatchMatMul Parameter Sweep Tests
//===--------------------------------------------------------------------===//

/// Create a simple network that has a single fp batch mat mul.
static FunctionTensorPair
createAndInitBatchMatMulNet(glow::PlaceholderBindings &bindings,
                            glow::ExecutionEngine &EE, size_t N, size_t A,
                            size_t Z, size_t B) {
  PseudoRNG PRNG;
  auto &mod = EE.getModule();
  Function *F = mod.createFunction("main");
  auto *LHS = mod.createPlaceholder(ElemKind::FloatTy, {N, A, Z}, "LHS", false);
  auto *RHS = mod.createPlaceholder(ElemKind::FloatTy, {N, Z, B}, "RHS", false);
  bindings.allocate(LHS)->getHandle().initXavier(10, PRNG);
  bindings.allocate(RHS)->getHandle().initXavier(10, PRNG);

  auto *R = F->createBatchMatMul("BMM", LHS, RHS);

  auto *save = F->createSave("save", R);
  auto *resultTensor = bindings.allocate(save->getPlaceholder());

  return std::make_pair(F, resultTensor);
}

/// Helper to test sweeping across a variety of configurations of a BatchMatMul
/// by comparing the results to the Interpreter given some \p allowedError.
/// \p config contains the backend to compare the Interpreter against, plus the
/// specific configuration to run for this test. \p interpK and \p backendK are
/// the element kinds to use for the Interpreter and backend, respectively.
static void testParamSweepBatchMatMul(ThreeIntTupleConfig config,
                                      ElemKind interpK, ElemKind backendK,
                                      float allowedError) {
  std::string backend;
  size_t N, A, Z;
  SET_BACKEND_KIND_AND_THREE_INT_PARAMS(config, backend, N, A, Z);
  size_t B = A;

  LOG(INFO) << "\n\tTesting BatchMatMul with N: " << N << "; A: " << A
            << "; Z: " << Z << "; B: " << B << "\n";

  // Multiplying LHS {N, A, Z} by RHS {N, Z, B} to get result {N, A, B}.
  auto boundF = std::bind(createAndInitBatchMatMulNet, std::placeholders::_1,
                          std::placeholders::_2, N, A, Z, B);
  compareAgainstInterpreter(backend, boundF, interpK, backendK, allowedError,
                            parCloneCountOpt);
}

DECLARE_STATELESS_BACKEND_TEST(BatchMatMulSweepTest, ThreeIntTupleConfig);

INSTANTIATE_TEST_CASE_P_FOR_BACKEND_COMBINED_TEST(
    SweepTest, BatchMatMulSweepTest,
    ::testing::Combine(/* N */ ::testing::Values(1, 4, 16, 24),
                       /* A */ ::testing::Range(10, 16),
                       /* Z */ ::testing::Values(32, 64, 128, 256)));

/// Compare backend against the interpreter in Float.
TEST_P(BatchMatMulSweepTest, BatchMatMulTest_Float) {
  ENABLED_BACKENDS(CPU, OpenCL);
  testParamSweepBatchMatMul(GetParam(), ElemKind::FloatTy, ElemKind::FloatTy,
                            0.0001f);
}

/// Compare backend against the interpreter in Int8.
TEST_P(BatchMatMulSweepTest, BatchMatMulTest_Int8) {
  ENABLED_BACKENDS(Interpreter, CPU, OpenCL);
  testParamSweepBatchMatMul(GetParam(), ElemKind::FloatTy, ElemKind::Int8QTy,
                            0.06f);
}

/// Compare backend against the interpreter in FP16.
TEST_P(BatchMatMulSweepTest, BatchMatMulTest_Float16) {
  ENABLED_BACKENDS(Interpreter);
  testParamSweepBatchMatMul(GetParam(), ElemKind::FloatTy, ElemKind::Float16Ty,
                            0.005f);
}

//===--------------------------------------------------------------------===//
//                   FullyConnected Parameter Sweep Tests
//===--------------------------------------------------------------------===//

/// Create a simple network that has a single fp FC.
static FunctionTensorPair
createAndInitFCNet(glow::PlaceholderBindings &bindings,
                   glow::ExecutionEngine &EE, size_t A, size_t Z, size_t B) {
  PseudoRNG PRNG;
  auto &mod = EE.getModule();
  Function *F = mod.createFunction("main");
  auto *IP = mod.createPlaceholder(ElemKind::FloatTy, {A, Z}, "input", false);
  auto *WC = mod.createConstant(ElemKind::FloatTy, {Z, B}, "weights");
  auto *BC = mod.createConstant(ElemKind::FloatTy, {B}, "bias");
  bindings.allocate(IP)->getHandle().randomize(-0.2, 0.2, mod.getPRNG());
  BC->getPayloadMutable().getHandle().randomize(0, 0.000005, mod.getPRNG());
  WC->getPayloadMutable().getHandle().randomize(-0.4, 0.4, mod.getPRNG());

  auto *FC = F->createFullyConnected("FC", IP, WC, BC);
  auto *save = F->createSave("save", FC);
  auto *resultTensor = bindings.allocate(save->getPlaceholder());

  return std::make_pair(F, resultTensor);
}

/// Helper to test sweeping across a variety of configurations of a FC by
/// comparing the results to the Interpreter given some \p allowedError.
/// \p config contains the backend to compare the Interpreter against, plus the
/// specific configuration to run for this test. \p interpK and \p backendK are
/// the element kinds to use for the Interpreter and backend, respectively.
static void testParamSweepFC(ThreeIntTupleConfig config, ElemKind interpK,
                             ElemKind backendK, float allowedError) {
  std::string backend;
  size_t A, Z, B;
  SET_BACKEND_KIND_AND_THREE_INT_PARAMS(config, backend, A, Z, B);

  LOG(INFO) << "\n\tTesting FC with A: " << A << "; Z: " << Z << "; B: " << B
            << "\n";

  auto boundF = std::bind(createAndInitFCNet, std::placeholders::_1,
                          std::placeholders::_2, A, Z, B);
  compareAgainstInterpreter(backend, boundF, interpK, backendK, allowedError,
                            parCloneCountOpt);
}

DECLARE_STATELESS_BACKEND_TEST(FCSweepTest, ThreeIntTupleConfig);

INSTANTIATE_TEST_CASE_P_FOR_BACKEND_COMBINED_TEST(
    SweepTest, FCSweepTest,
    ::testing::Combine(/* A */ ::testing::Values(1, 4, 16, 64),
                       /* Z */ ::testing::Values(256, 512, 1024, 2048, 4096),
                       /* B */ ::testing::Values(64, 256, 1024)));

/// Compare backend against the interpreter in Float.
TEST_P(FCSweepTest, FCTest_Float) {
  ENABLED_BACKENDS(CPU, OpenCL);
  testParamSweepFC(GetParam(), ElemKind::FloatTy, ElemKind::FloatTy, 0.0001f);
}

/// Compare backend against the interpreter in Int8.
TEST_P(FCSweepTest, FCTest_Int8) {
  ENABLED_BACKENDS(Interpreter, CPU, OpenCL);
  testParamSweepFC(GetParam(), ElemKind::FloatTy, ElemKind::Int8QTy, 0.065f);
}

/// Compare backend against the interpreter in FP16.
TEST_P(FCSweepTest, FCTest_Float16) {
  ENABLED_BACKENDS(Interpreter);
  testParamSweepFC(GetParam(), ElemKind::FloatTy, ElemKind::Float16Ty, 0.004f);
}