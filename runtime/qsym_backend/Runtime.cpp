// This file is part of the SymCC runtime.
//
// The SymCC runtime is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// The SymCC runtime is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with SymCC. If not, see <https://www.gnu.org/licenses/>.

//
// Definitions that we need for the QSYM backend
//

#include "Runtime.h"
#include "GarbageCollection.h"
#include <algorithm>
#include <cstddef>
#include <dependency.h>
#include <expr.h>
#include <utility>

// C++
#if __has_include(<filesystem>)
#define HAVE_FILESYSTEM 1
#elif __has_include(<experimental/filesystem>)
#define HAVE_FILESYSTEM 0
#else
#error "We need either <filesystem> or the older <experimental/filesystem>."
#endif

#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_set>
#include <variant>

#if HAVE_FILESYSTEM
#include <filesystem>
#else
#include <experimental/filesystem>
#endif

#ifdef DEBUG_RUNTIME
#include <chrono>
#endif

// C
#include <cstdint>
#include <cstdio>

// QSYM
#include <afl_trace_map.h>
#include <call_stack_manager.h>
#include <expr_builder.h>
#include <solver.h>

// LLVM
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>

// Runtime
#include <Config.h>
#include <LibcWrappers.h>
#include <Shadow.h>

namespace qsym {

ExprBuilder *g_expr_builder;
Solver *g_solver;
CallStackManager g_call_stack_manager;
z3::context *g_z3_context;

} // namespace qsym

namespace {

/// Indicate whether the runtime has been initialized.
std::atomic_flag g_initialized = ATOMIC_FLAG_INIT;

/// A mapping of all expressions that we have ever received from QSYM to the
/// corresponding shared pointers on the heap.
///
/// We can't expect C clients to handle std::shared_ptr, so we maintain a single
/// copy per expression in order to keep the expression alive. The garbage
/// collector decides when to release our shared pointer.
///
/// std::map seems to perform slightly better than std::unordered_map on our
/// workload.
std::map<SymExpr, qsym::ExprRef> allocatedExpressions;

// TODO lack Garbage collection, may cause occupy large memory
std::unordered_map<qsym::DependencySet*, std::pair<SymExpr, uintptr_t>> g_delay_constraint_queue;
qsym::DependencySet g_exact_dependencies;

SymExpr registerExpression(const qsym::ExprRef &expr) {
  SymExpr rawExpr = expr.get();

  if (allocatedExpressions.count(rawExpr) == 0) {
    // We don't know this expression yet. Create a copy of the shared pointer to
    // keep the expression alive.
    allocatedExpressions[rawExpr] = expr;
  }

  return rawExpr;
}

/// The user-provided test case handler, if any.
///
/// If the user doesn't register a handler, we use QSYM's default behavior of
/// writing the test case to a file in the output directory.
TestCaseHandler g_test_case_handler = nullptr;

/// A QSYM solver that doesn't require the entire input on initialization.
class EnhancedQsymSolver : public qsym::Solver {
  // Warning!
  //
  // Before we can override methods of qsym::Solver, we need to declare them
  // virtual because the QSYM code refers to the solver with a pointer of type
  // qsym::Solver*; for non-virtual methods, it will always choose the
  // implementation of the base class. Beware that making a method virtual adds
  // a small performance overhead and requires us to change QSYM code.
  //
  // Subclassing the QSYM solver is ugly but helps us to avoid making too many
  // changes in the QSYM codebase.

public:
  EnhancedQsymSolver()
      : qsym::Solver("/dev/null", g_config.outputDir, g_config.aflCoverageMap) {
  }

  void pushInputByte(size_t offset, uint8_t value) {
    if (inputs_.size() <= offset)
      inputs_.resize(offset + 1);

    inputs_[offset] = value;
  }

  void saveValues(const std::string &suffix) override {
    if (auto handler = g_test_case_handler) {
      auto values = getConcreteValues();
      handler(values.data(), values.size());
    } else {
      Solver::saveValues(suffix);
    }
  }
};

EnhancedQsymSolver *g_enhanced_solver;

} // namespace

using namespace qsym;

#if HAVE_FILESYSTEM
namespace fs = std::filesystem;
#else
namespace fs = std::experimental::filesystem;
#endif

void _sym_initialize(void) {
  if (g_initialized.test_and_set())
    return;

  loadConfig();
  initLibcWrappers();
  std::cerr << "This is SymCC running with the QSYM backend" << std::endl;
  if (std::holds_alternative<NoInput>(g_config.input)) {
    std::cerr
        << "Performing fully concrete execution (i.e., without symbolic input)"
        << std::endl;
    return;
  }

  // Check the output directory
  if (!fs::exists(g_config.outputDir) ||
      !fs::is_directory(g_config.outputDir)) {
    std::cerr << "Error: the output directory " << g_config.outputDir
              << " (configurable via SYMCC_OUTPUT_DIR) does not exist."
              << std::endl;
    exit(-1);
  }

  g_z3_context = new z3::context{};
  g_enhanced_solver = new EnhancedQsymSolver{};
  g_solver = g_enhanced_solver; // for QSYM-internal use
  g_expr_builder = g_config.pruning ? PruneExprBuilder::create()
                                    : SymbolicExprBuilder::create();
}

SymExpr _sym_build_integer(uint64_t value, uint8_t bits) {
  // QSYM's API takes uintptr_t, so we need to be careful when compiling for
  // 32-bit systems: the compiler would helpfully truncate our uint64_t to fit
  // into 32 bits.
  if constexpr (sizeof(uint64_t) == sizeof(uintptr_t)) {
    // 64-bit case: all good.
    return registerExpression(g_expr_builder->createConstant(value, bits));
  } else {
    // 32-bit case: use the regular API if possible, otherwise create an
    // llvm::APInt.
    if (uintptr_t value32 = value; value32 == value)
      return registerExpression(g_expr_builder->createConstant(value32, bits));

    return registerExpression(
        g_expr_builder->createConstant({64, value}, bits));
  }
}

SymExpr _sym_build_integer128(uint64_t high, uint64_t low) {
  std::array<uint64_t, 2> words = {low, high};
  return registerExpression(g_expr_builder->createConstant({128, words}, 128));
}

SymExpr _sym_build_null_pointer() {
  return registerExpression(
      g_expr_builder->createConstant(0, sizeof(uintptr_t) * 8));
}

SymExpr _sym_build_true() {
  return registerExpression(g_expr_builder->createTrue());
}

SymExpr _sym_build_false() {
  return registerExpression(g_expr_builder->createFalse());
}

SymExpr _sym_build_bool(bool value) {
  return registerExpression(g_expr_builder->createBool(value));
}

#define DEF_BINARY_EXPR_BUILDER(name, qsymName)                                \
  SymExpr _sym_build_##name(SymExpr a, SymExpr b) {                            \
    return registerExpression(g_expr_builder->create##qsymName(                \
        allocatedExpressions.at(a), allocatedExpressions.at(b)));              \
  }

DEF_BINARY_EXPR_BUILDER(add, Add)
DEF_BINARY_EXPR_BUILDER(sub, Sub)
DEF_BINARY_EXPR_BUILDER(mul, Mul)
DEF_BINARY_EXPR_BUILDER(unsigned_div, UDiv)
DEF_BINARY_EXPR_BUILDER(signed_div, SDiv)
DEF_BINARY_EXPR_BUILDER(unsigned_rem, URem)
DEF_BINARY_EXPR_BUILDER(signed_rem, SRem)

DEF_BINARY_EXPR_BUILDER(shift_left, Shl)
DEF_BINARY_EXPR_BUILDER(logical_shift_right, LShr)
DEF_BINARY_EXPR_BUILDER(arithmetic_shift_right, AShr)

DEF_BINARY_EXPR_BUILDER(signed_less_than, Slt)
DEF_BINARY_EXPR_BUILDER(signed_less_equal, Sle)
DEF_BINARY_EXPR_BUILDER(signed_greater_than, Sgt)
DEF_BINARY_EXPR_BUILDER(signed_greater_equal, Sge)
DEF_BINARY_EXPR_BUILDER(unsigned_less_than, Ult)
DEF_BINARY_EXPR_BUILDER(unsigned_less_equal, Ule)
DEF_BINARY_EXPR_BUILDER(unsigned_greater_than, Ugt)
DEF_BINARY_EXPR_BUILDER(unsigned_greater_equal, Uge)
DEF_BINARY_EXPR_BUILDER(equal, Equal)
DEF_BINARY_EXPR_BUILDER(not_equal, Distinct)

DEF_BINARY_EXPR_BUILDER(bool_and, LAnd)
DEF_BINARY_EXPR_BUILDER(and, And)
DEF_BINARY_EXPR_BUILDER(bool_or, LOr)
DEF_BINARY_EXPR_BUILDER(or, Or)
DEF_BINARY_EXPR_BUILDER(bool_xor, Distinct)
DEF_BINARY_EXPR_BUILDER(xor, Xor)

#undef DEF_BINARY_EXPR_BUILDER

SymExpr _sym_build_neg(SymExpr expr) {
  return registerExpression(
      g_expr_builder->createNeg(allocatedExpressions.at(expr)));
}

SymExpr _sym_build_not(SymExpr expr) {
  return registerExpression(
      g_expr_builder->createNot(allocatedExpressions.at(expr)));
}

SymExpr _sym_build_ite(SymExpr cond, SymExpr a, SymExpr b) {
  return registerExpression(g_expr_builder->createIte(
      allocatedExpressions.at(cond), allocatedExpressions.at(a),
      allocatedExpressions.at(b)));
}

SymExpr _sym_build_sext(SymExpr expr, uint8_t bits) {
  if (expr == nullptr)
    return nullptr;

  return registerExpression(g_expr_builder->createSExt(
      allocatedExpressions.at(expr), bits + expr->bits()));
}

SymExpr _sym_build_zext(SymExpr expr, uint8_t bits) {
  if (expr == nullptr)
    return nullptr;

  return registerExpression(g_expr_builder->createZExt(
      allocatedExpressions.at(expr), bits + expr->bits()));
}

SymExpr _sym_build_trunc(SymExpr expr, uint8_t bits) {
  if (expr == nullptr)
    return nullptr;

  return registerExpression(
      g_expr_builder->createTrunc(allocatedExpressions.at(expr), bits));
}

void _sym_push_path_constraint(SymExpr constraint, int taken,
                               uintptr_t site_id) {


  if (constraint == nullptr)
    return;

#ifdef WITH_SANITIZER_RUNTIME
  // printf("\nPush Constaint:%s\n, taken:%d\n", _sym_expr_to_string(constraint), taken);
  g_solver->addJcc(allocatedExpressions.at(constraint), taken != 0, site_id, false);
#else
  g_solver->addJcc(allocatedExpressions.at(constraint), taken != 0, site_id);
#endif
}

#ifdef WITH_SANITIZER_RUNTIME
void _sym_asan_push_path_constraint(SymExpr constraint, int taken, uintptr_t site_id) {
  if (constraint == nullptr)
    return;

  g_solver->addJcc(allocatedExpressions.at(constraint), taken != 0, site_id, true);
}

void _sym_asan_test_dependency(SymExpr constraint) {
  ExprRef node = allocatedExpressions.at(constraint);
  printf("DependencySet-------\n");
  for (auto &index : *node->getDependencies()) {
    printf("%ld\n", index);
  }
  printf("DependencySet End-------\n");
}

void _sym_asan_insert_symbolic_addr_node(SymExpr value, SymExpr addr, uintptr_t concrete_addr) {
  ExprRef node = allocatedExpressions.at(value);
  DependencySet *dep = node->getDependencies();
  if (std::includes(g_exact_dependencies.begin(), g_exact_dependencies.end(), dep->begin(), dep->end())) return;
  // check for the repeat dependency, may introduce large overhead
  for(auto iter = g_delay_constraint_queue.begin(); iter != g_delay_constraint_queue.end(); iter++) {
    DependencySet iter_dep = *iter->first;
    if (std::includes(iter_dep.begin(), iter_dep.end(), dep->begin(), dep->end())) return;
  }
  g_delay_constraint_queue.insert({dep, make_pair(addr, concrete_addr)});
}

void _sym_asan_constraint_verify(SymExpr expr) {
  if (g_delay_constraint_queue.empty()) return;
  ExprRef node = allocatedExpressions.at(expr);
  DependencySet br_dep = *node->getDependencies(); 
  for(auto iter = g_delay_constraint_queue.begin(); iter != g_delay_constraint_queue.end(); iter++) {
    DependencySet dep = *iter->first;
    if (!std::includes(br_dep.begin(), br_dep.end(), dep.begin(), dep.end())) continue;
    SymExpr addr_sym = iter->second.first;
    uintptr_t addr_con = iter->second.second;
    _sym_push_path_constraint(_sym_build_equal(_sym_build_integer(addr_con, 64), addr_sym), 1, 0);
    g_exact_dependencies.merge(dep);
    g_delay_constraint_queue.erase(iter);
    break;
  }
}

bool _sym_asan_is_symexpr_exact(SymExpr expr) {
  if (g_exact_dependencies.empty()) return false;
  DependencySet dep = *expr->getDependencies();
  if (std::includes(g_exact_dependencies.begin(), g_exact_dependencies.end(), dep.begin(), dep.end()))
    return true;
  else
    return false;
}

#endif

SymExpr _sym_get_input_byte(size_t offset, uint8_t value) {
  g_enhanced_solver->pushInputByte(offset, value);
  return registerExpression(g_expr_builder->createRead(offset));
}

SymExpr _sym_concat_helper(SymExpr a, SymExpr b) {
  return registerExpression(g_expr_builder->createConcat(
      allocatedExpressions.at(a), allocatedExpressions.at(b)));
}

SymExpr _sym_extract_helper(SymExpr expr, size_t first_bit, size_t last_bit) {
  return registerExpression(g_expr_builder->createExtract(
      allocatedExpressions.at(expr), last_bit, first_bit - last_bit + 1));
}

size_t _sym_bits_helper(SymExpr expr) { return expr->bits(); }

SymExpr _sym_build_bool_to_bit(SymExpr expr) {
  if (expr == nullptr)
    return nullptr;

  return registerExpression(
      g_expr_builder->boolToBit(allocatedExpressions.at(expr), 1));
}

//
// Floating-point operations (unsupported in QSYM)
//

#define UNSUPPORTED(prototype)                                                 \
  prototype { return nullptr; }

UNSUPPORTED(SymExpr _sym_build_float(double, int))
UNSUPPORTED(SymExpr _sym_build_fp_add(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_fp_sub(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_fp_mul(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_fp_div(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_fp_rem(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_fp_abs(SymExpr))
UNSUPPORTED(SymExpr _sym_build_fp_neg(SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_ordered_greater_than(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_ordered_greater_equal(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_ordered_less_than(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_ordered_less_equal(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_ordered_equal(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_ordered_not_equal(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_ordered(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_unordered(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_unordered_greater_than(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_unordered_greater_equal(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_unordered_less_than(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_unordered_less_equal(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_unordered_equal(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_unordered_not_equal(SymExpr, SymExpr))
UNSUPPORTED(SymExpr _sym_build_int_to_float(SymExpr, int, int))
UNSUPPORTED(SymExpr _sym_build_float_to_float(SymExpr, int))
UNSUPPORTED(SymExpr _sym_build_bits_to_float(SymExpr, int))
UNSUPPORTED(SymExpr _sym_build_float_to_bits(SymExpr))
UNSUPPORTED(SymExpr _sym_build_float_to_signed_integer(SymExpr, uint8_t))
UNSUPPORTED(SymExpr _sym_build_float_to_unsigned_integer(SymExpr, uint8_t))

#undef UNSUPPORTED
#undef H

//
// Call-stack tracing
//

void _sym_notify_call(uintptr_t site_id) {
  g_call_stack_manager.visitCall(site_id);
}

void _sym_notify_ret(uintptr_t site_id) {
  g_call_stack_manager.visitRet(site_id);
}

void _sym_notify_basic_block(uintptr_t site_id) {
  g_call_stack_manager.visitBasicBlock(site_id);
}

//
// Debugging
//

const char *_sym_expr_to_string(SymExpr expr) {
  static char buffer[4096];

  auto expr_string = expr->toString();
  auto copied = expr_string.copy(
      buffer, std::min(expr_string.length(), sizeof(buffer) - 1));
  buffer[copied] = '\0';

  return buffer;
}

bool _sym_feasible(SymExpr expr) {
  expr->simplify();

  g_solver->push();
  g_solver->add(expr->toZ3Expr());
  bool feasible = (g_solver->check() == z3::sat);
  g_solver->pop();

  return feasible;
}

//
// Garbage collection
//

void _sym_collect_garbage() {
  if (allocatedExpressions.size() < g_config.garbageCollectionThreshold)
    return;

#ifdef DEBUG_RUNTIME
  auto start = std::chrono::high_resolution_clock::now();
#endif

  auto reachableExpressions = collectReachableExpressions();
  for (auto expr_it = allocatedExpressions.begin();
       expr_it != allocatedExpressions.end();) {
    if (reachableExpressions.count(expr_it->first) == 0) {
      expr_it = allocatedExpressions.erase(expr_it);
    } else {
      ++expr_it;
    }
  }

#ifdef DEBUG_RUNTIME
  auto end = std::chrono::high_resolution_clock::now();

  std::cerr << "After garbage collection: " << allocatedExpressions.size()
            << " expressions remain" << std::endl
            << "\t(collection took "
            << std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                     start)
                   .count()
            << " milliseconds)" << std::endl;
#endif
}

//
// Test-case handling
//

void symcc_set_test_case_handler(TestCaseHandler handler) {
  g_test_case_handler = handler;
}
