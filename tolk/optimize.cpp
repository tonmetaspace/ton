/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "tolk.h"

namespace tolk {

/*
 * 
 *   PEEPHOLE OPTIMIZER
 * 
 */

void Optimizer::set_code(AsmOpConsList code) {
  code_ = std::move(code);
  unpack();
}

void Optimizer::unpack() {
  int i = 0, j = 0;
  for (AsmOpCons *p = code_.get(); p && i < optimize_depth; p = p->cdr.get(), ++j) {
    if (p->car->is_very_custom()) {
      break;
    }
    if (p->car->is_comment()) {
      continue;
    }
    op_cons_[i] = p;
    op_[i] = std::move(p->car);
    offs_[i] = j;
    ++i;
  }
  l_ = i;
  indent_ = (i ? op_[0]->indent : 0);
}

void Optimizer::pack() {
  for (int i = 0; i < l_; i++) {
    op_cons_[i]->car = std::move(op_[i]);
    op_cons_[i] = nullptr;
  }
  l_ = 0;
}

void Optimizer::apply() {
  if (!p_ && !q_) {
    return;
  }
  tolk_assert(p_ > 0 && p_ <= l_ && q_ >= 0 && q_ <= optimize_depth && l_ <= optimize_depth);
  for (int i = p_; i < l_; i++) {
    tolk_assert(op_[i]);
    op_cons_[i]->car = std::move(op_[i]);
    op_cons_[i] = nullptr;
  }
  for (int c = offs_[p_ - 1]; c >= 0; --c) {
    code_ = std::move(code_->cdr);
  }
  for (int j = q_ - 1; j >= 0; j--) {
    tolk_assert(oq_[j]);
    oq_[j]->indent = indent_;
    code_ = AsmOpCons::cons(std::move(oq_[j]), std::move(code_));
  }
  l_ = 0;
}

AsmOpConsList Optimizer::extract_code() {
  pack();
  return std::move(code_);
}

void Optimizer::show_head() const {
  if (!debug_) {
    return;
  }
  std::cerr << "optimizing";
  for (int i = 0; i < l_; i++) {
    if (op_[i]) {
      std::cerr << ' ' << *op_[i] << ' ';
    } else {
      std::cerr << " (null) ";
    }
  }
  std::cerr << std::endl;
}

void Optimizer::show_left() const {
  if (!debug_) {
    return;
  }
  std::cerr << "// *** rewriting";
  for (int i = 0; i < p_; i++) {
    if (op_[i]) {
      std::cerr << ' ' << *op_[i] << ' ';
    } else {
      std::cerr << " (null) ";
    }
  }
}

void Optimizer::show_right() const {
  if (!debug_) {
    return;
  }
  std::cerr << "->";
  for (int i = 0; i < q_; i++) {
    if (oq_[i]) {
      std::cerr << ' ' << *oq_[i] << ' ';
    } else {
      std::cerr << " (null) ";
    }
  }
  std::cerr << std::endl;
}

bool Optimizer::find_const_op(int* op_idx, int cst) {
  for (int i = 0; i < l2_; i++) {
    if (op_[i]->is_gconst() && tr_[i].get(0) == cst) {
      *op_idx = i;
      return true;
    }
  }
  return false;
}

// purpose: transform `65535 THROW` to `PUSHINT` + `THROWANY`;
// such a technique allows pushing a number onto a stack just before THROW, even if a variable is created in advance;
// used for `T.fromSlice(s, {code:0xFFFF})`, where `tmp = 0xFFFF` + serialization match + `else throw tmp` is generated;
// but since it's constant, it transforms to (unused 0xFFFF) + ... + else "65535 THROW", unwrapped here
bool Optimizer::detect_rewrite_big_THROW() {
  bool is_throw = op_[0]->is_custom() && op_[0]->op.ends_with(" THROW");
  if (!is_throw) {
    return false;
  }

  std::string_view s_num_throw = op_[0]->op;
  size_t sp = s_num_throw.find(' ');
  if (sp != s_num_throw.rfind(' ') || s_num_throw[0] < '1' || s_num_throw[0] > '9') {
    return false;
  }

  std::string s_number(s_num_throw.substr(0, sp));
  uint64_t excno = std::stoul(s_number);
  if (excno < 2048) {   // "9 THROW" left as is, but "N THROW" where N>=2^11 is invalid for Fift
    return false;
  }

  p_ = 1;
  q_ = 2;
  oq_[0] = std::make_unique<AsmOp>(AsmOp::IntConst(op_[0]->loc, td::make_refint(excno)));
  oq_[1] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, "THROWANY", 1, 0));
  return true;
}

// purpose 1: for b.storeInt(123, 32) generate not "123 PUSHINT; SWAP; STI", but "x{...} STSLICECONST"
// purpose 2: consecutive b.storeUint(ff, 16).storeUint(ff, 16) generate one "x{00ff00ff} STSLICECONST"
// (since it works at IR level, it also works for const variables and auto-serialization)
bool Optimizer::detect_rewrite_MY_store_int() {
  bool first_my_store = op_[0]->is_custom() && op_[0]->op.starts_with("MY_store_int");
  if (!first_my_store) {
    return false;
  }
  bool first_unsigned = op_[0]->op[12] == 'U';

  int n_merged = 0;
  td::RefInt256 total_number = td::make_refint(0);
  int total_len = 0;
  for (int i = 0; i < pb_; ++i) {
    std::string_view s_op_number_len = op_[i]->op;    // "MY_store_intU 123 32"
    if (!s_op_number_len.starts_with("MY_store_int")) {
      break;
    }

    size_t sp = s_op_number_len.rfind(' ');
    std::string s_number(s_op_number_len.substr(13 + 1, sp - 13 - 1));
    int len = std::stoi(std::string(s_op_number_len.substr(sp + 1)));

    if (total_len + len > (255 + first_unsigned)) {
      break;
    }
    if (total_number != 0) {
      total_number <<= len;
    }
    total_number += td::string_to_int256(s_number);
    total_len += len;
    n_merged++;
  }

  // we do not want to always use STSLICECONST; for example, storing "0" 64-bit via x{00...} is more effective
  // for a single operation, but in practice, total bytecode becomes larger, which has a cumulative negative effect;
  // here is a heuristic "when to use STSLICECONST, when leave PUSHINT + STUR", based on real contracts measurements
  bool use_stsliceconst = total_len <= 32 || (total_len <= 48 && total_number >= 256) || (total_len <= 64 && total_number >= 65536)
                      || (total_len <= 96 && total_number >= (1ULL<<32)) || (total_number > (1ULL<<62));
  if (!use_stsliceconst) {
    p_ = n_merged;
    q_ = 2;
    oq_[0] = std::make_unique<AsmOp>(AsmOp::IntConst(op_[0]->loc, total_number));
    oq_[1] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, std::to_string(total_len) + (first_unsigned ? " STUR" : " STIR"), 1, 1));
    return true;
  }

  p_ = n_merged;
  q_ = 1;

  // output "x{...}" or "b{...}" (if length not divisible by 4)
  const td::RefInt256 base = td::make_refint(total_len % 4 == 0 ? 16 : 2);
  const int s_len = base == 16 ? total_len / 4 : total_len;
  const char* digits = "0123456789abcdef";

  std::string result(s_len + 3, '0');
  result[0] = base == 16 ? 'x' : 'b';
  result[1] = '{';
  result[s_len + 3 - 1] = '}';
  for (int i = s_len - 1; i >= 0 && total_number != 0; --i) {
    result[2 + i] = digits[(total_number % base)->to_long()];
    total_number /= base;
  }

  result += " STSLICECONST";
  oq_[0] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, result, 0, 1));
  return true;
}

// purpose: consecutive `s.skipBits(8).skipBits(const_var_16)` will be joined into a single 24
bool Optimizer::detect_rewrite_MY_skip_bits() {
  bool first_my_skip = op_[0]->is_custom() && op_[0]->op.starts_with("MY_skip_bits");
  if (!first_my_skip) {
    return false;
  }

  int n_merged = 0;
  int total_skip_bits = 0;
  for (int i = 0; i < pb_; ++i) {
    std::string_view s_op_len = op_[i]->op;       // "MY_skip_bits 32"
    if (!s_op_len.starts_with("MY_skip_bits")) {
      break;
    }

    std::string s_number(s_op_len.substr(s_op_len.find(' ') + 1));
    total_skip_bits += std::stoi(s_number);
    n_merged++;
  }

  p_ = n_merged;
  q_ = 2;
  if (total_skip_bits <= 256) {
    oq_[0] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, std::to_string(total_skip_bits) + " LDU"));
    oq_[1] = std::make_unique<AsmOp>(AsmOp::Pop(op_[0]->loc, 1));
  } else {
    oq_[0] = std::make_unique<AsmOp>(AsmOp::IntConst(op_[0]->loc, td::make_refint(total_skip_bits)));
    oq_[1] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, "SDSKIPFIRST"));
  }
  return true;
}

// pattern `NEWC` + `xxx PUSHINT` + `32 STUR` -> `xxx PUSHINT` + `NEWC` + `32 STU`, it's a bit cheaper
bool Optimizer::detect_rewrite_NEWC_PUSH_STUR() {
  bool first_newc = op_[0]->is_custom() && op_[0]->op == "NEWC";
  if (!first_newc || pb_ < 3) {
    return false;
  }
  bool next_push = op_[1]->is_const() && op_[1]->op.ends_with(" PUSHINT");  // actually there can be PUSHPOWDEC2, but ok
  if (!next_push) {
    return false;
  }
  bool next_stu_r = op_[2]->is_custom() && (op_[2]->op.ends_with(" STUR") || op_[2]->op.ends_with(" STIR"));
  if (!next_stu_r) {
    return false;
  }

  p_ = 3;
  q_ = 3;
  oq_[0] = std::move(op_[1]);
  oq_[1] = std::move(op_[0]);
  oq_[2] = std::make_unique<AsmOp>(AsmOp::Custom(oq_[0]->loc, op_[2]->op.substr(0, op_[2]->op.size() - 1), 1, 1));
  return true;
}

// pattern `N LDU` + `DROP` -> `N PLDU` (common after loading the last field manually or by `lazy`);
// the same for LDI -> PLDI, LDREF -> PLDREF, etc.
bool Optimizer::detect_rewrite_LDxx_DROP() {
  bool second_drop = pb_ > 1 && op_[1]->is_pop() && op_[1]->a == 0;
  if (!second_drop || !op_[0]->is_custom()) {
    return false;
  }

  static const char* ends_with[] = { " LDI",  " LDU",  " LDBITS"};
  static const char* repl_with[] = {" PLDI", " PLDU", " PLDBITS"};
  static const char* equl_to[] = { "LDREF",  "LDDICT",  "LDOPTREF",  "LDSLICEX"};
  static const char* repl_to[] = {"PLDREF", "PLDDICT", "PLDOPTREF", "PLDSLICEX"};

  std::string_view f = op_[0]->op;
  for (size_t i = 0; i < std::size(ends_with); ++i) {
    if (f.ends_with(ends_with[i])) {
      p_ = 2;
      q_ = 1;
      oq_[0] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, op_[0]->op.substr(0, f.rfind(' ')) + repl_with[i], 0, 1));
      return true;
    }
  }
  for (size_t i = 0; i < std::size(equl_to); ++i) {
    if (f == equl_to[i]) {
      p_ = 2;
      q_ = 1;
      oq_[0] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, repl_to[i], 0, 1));
      return true;
    }
  }

  return false;
}

// pattern `SWAP` + `EQUAL` -> `EQUAL`
// and other symmetric operators: NEQ, MUL, etc.
bool Optimizer::detect_rewrite_SWAP_symmetric() {
  bool first_swap = op_[0]->is_swap();
  if (!first_swap || pb_ < 2 || !op_[1]->is_custom()) {
    return false;
  }
  std::string_view n = op_[1]->op;
  bool next_symmetric = n == "EQUAL" || n == "NEQ" || n == "SDEQ" || n == "AND" || n == "OR"
                     || n == "ADD" || n == "MUL" || n == "MIN" || n == "MAX";
  if (!next_symmetric) {
    return false;
  }

  p_ = 2;
  q_ = 1;
  oq_[0] = std::move(op_[1]);
  return true;
}

// pattern `SWAP` + `xxx PUSHINT` + `32 STUR` -> `xxx PUSHINT` + `ROT` + `32 STU`
bool Optimizer::detect_rewrite_SWAP_PUSH_STUR() {
  bool first_swap = op_[0]->is_swap();
  if (!first_swap || pb_ < 3) {
    return false;
  }
  bool next_push = op_[1]->is_const() && op_[1]->op.ends_with(" PUSHINT");
  if (!next_push) {
    return false;
  }
  bool next_stu_r = op_[2]->is_custom() && (op_[2]->op.ends_with(" STUR") || op_[2]->op.ends_with(" STIR"));
  if (!next_stu_r) {
    return false;
  }

  p_ = 3;
  q_ = 3;
  oq_[0] = std::move(op_[1]);
  oq_[1] = std::make_unique<AsmOp>(AsmOp::BlkSwap(oq_[0]->loc, 1, 2));     // ROT
  oq_[2] = std::make_unique<AsmOp>(AsmOp::Custom(oq_[0]->loc,  op_[2]->op.substr(0, op_[2]->op.size() - 1), 1, 1));
  return true;
}

// pattern `SWAP` + `STSLICER` -> `STSLICE` and vice versa: `SWAP` + `STSLICE` => `STSLICER`;
// same for `STB` / `STREF` / `n STU` / `n STI`
bool Optimizer::detect_rewrite_SWAP_STxxxR() {
  bool first_swap = op_[0]->is_swap();
  if (!first_swap || pb_ < 2 || !op_[1]->is_custom()) {
    return false;
  }

  static const char* ends_with[] = {" STU",  " STI",  " STUR", " STIR"};
  static const char* repl_with[] = {" STUR", " STIR", " STU",  " STI"};
  static const char* equl_to[] = {"STSLICE",  "STSLICER",  "STB",  "STBR", "SUB",  "SUBR", "STREF",  "STREFR", "LESS",    "LEQ", "GREATER", "GEQ"};
  static const char* repl_to[] = {"STSLICER", "STSLICE",   "STBR", "STB",  "SUBR", "SUB",  "STREFR", "STREF",  "GREATER", "GEQ", "LESS",    "LEQ"};

  std::string_view f = op_[1]->op;
  for (size_t i = 0; i < std::size(ends_with); ++i) {
    if (f.ends_with(ends_with[i])) {
      p_ = 2;
      q_ = 1;
      oq_[0] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, op_[1]->op.substr(0, f.rfind(' ')) + repl_with[i], 1, 1));
      return true;
    }
  }
  for (size_t i = 0; i < std::size(equl_to); ++i) {
    if (f == equl_to[i]) {
      p_ = 2;
      q_ = 1;
      oq_[0] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, repl_to[i], 0, 1));
      return true;
    }
  }

  return false;
}

// pattern `NOT` + `123 THROWIFNOT` -> `123 THROWIF` (and THROWIF -> THROWIFNOT)
bool Optimizer::detect_rewrite_NOT_THROWIF() {
  bool first_not = op_[0]->is_custom() && op_[0]->op == "NOT";
  if (!first_not || pb_ < 2 || !op_[1]->is_custom()) {
    return false;
  }

  static const char* ends_with[] = {" THROWIF",    " THROWIFNOT"};
  static const char* repl_with[] = {" THROWIFNOT", " THROWIF"};

  std::string_view f = op_[1]->op;
  for (size_t i = 0; i < std::size(ends_with); ++i) {
    if (f.ends_with(ends_with[i])) {
      p_ = 2;
      q_ = 1;
      oq_[0] = std::make_unique<AsmOp>(AsmOp::Custom(op_[0]->loc, op_[1]->op.substr(0, f.rfind(' ')) + repl_with[i], 1, 0));
      return true;
    }
  }

  return false;
}

bool Optimizer::is_push_const(int* i, int* c) const {
  return pb_ >= 3 && pb_ <= l2_ && tr_[pb_ - 1].is_push_const(i, c);
}

// PUSHCONST c ; PUSH s(i+1) ; SWAP -> PUSH s(i) ; PUSHCONST c
bool Optimizer::rewrite_push_const(int i, int c) {
  p_ = pb_;
  q_ = 2;
  int idx = -1;
  if (!(p_ >= 2 && find_const_op(&idx, c) && idx < p_)) {
    return false;
  }
  show_left();
  oq_[1] = std::move(op_[idx]);
  oq_[0] = std::move(op_[!idx]);
  *oq_[0] = AsmOp::Push(oq_[0]->loc, i);
  show_right();
  return true;
}

bool Optimizer::is_const_rot(int* c) const {
  return pb_ >= 3 && pb_ <= l2_ && tr_[pb_ - 1].is_const_rot(c);
}

bool Optimizer::rewrite_const_rot(int c) {
  p_ = pb_;
  q_ = 2;
  int idx = -1;
  if (!(p_ >= 2 && find_const_op(&idx, c) && idx < p_)) {
    return false;
  }
  show_left();
  oq_[0] = std::move(op_[idx]);
  oq_[1] = std::move(op_[!idx]);
  *oq_[1] = AsmOp::Custom(oq_[0]->loc, "ROT", 3, 3);
  show_right();
  return true;
}

bool Optimizer::is_const_pop(int* c, int* i) const {
  return pb_ >= 3 && pb_ <= l2_ && tr_[pb_ - 1].is_const_pop(c, i);
}

bool Optimizer::rewrite_const_pop(int c, int i) {
  p_ = pb_;
  q_ = 2;
  int idx = -1;
  if (!(p_ >= 2 && find_const_op(&idx, c) && idx < p_)) {
    return false;
  }
  show_left();
  oq_[0] = std::move(op_[idx]);
  oq_[1] = std::move(op_[!idx]);
  *oq_[1] = AsmOp::Pop(oq_[0]->loc, i);
  show_right();
  return true;
}

bool Optimizer::is_const_push_xchgs() {
  if (!(pb_ >= 2 && pb_ <= l2_ && op_[0]->is_gconst())) {
    return false;
  }
  StackTransform t;
  int pos = 0, i;
  for (i = 1; i < pb_; i++) {
    int a, b;
    if (op_[i]->is_xchg(&a, &b)) {
      if (pos == a) {
        pos = b;
      } else if (pos == b) {
        pos = a;
      } else {
        t.apply_xchg(a - (a > pos), b - (b > pos));
      }
    } else if (op_[i]->is_push(&a)) {
      if (pos == a) {
        return false;
      }
      t.apply_push(a - (a > pos));
      ++pos;
    } else {
      return false;
    }
  }
  if (pos) {
    return false;
  }
  t.apply_push_newconst();
  if (t <= tr_[i - 1]) {
    p_ = i;
    return true;
  } else {
    return false;
  }
}

bool Optimizer::rewrite_const_push_xchgs() {
  if (!p_) {
    return false;
  }
  show_left();
  auto c_op = std::move(op_[0]);
  tolk_assert(c_op->is_gconst());
  StackTransform t;
  q_ = 0;
  int pos = 0;
  for (int i = 1; i < p_; i++) {
    int a, b;
    if (op_[i]->is_xchg(&a, &b)) {
      if (a == pos) {
        pos = b;
      } else if (b == pos) {
        pos = a;
      } else {
        oq_[q_] = std::move(op_[i]);
        if (a > pos) {
          oq_[q_]->a = a - 1;
        }
        if (b > pos) {
          oq_[q_]->b = b - 1;
        }
        tolk_assert(apply_op(t, *oq_[q_]));
        ++q_;
      }
    } else {
      tolk_assert(op_[i]->is_push(&a));
      tolk_assert(a != pos);
      oq_[q_] = std::move(op_[i]);
      if (a > pos) {
        oq_[q_]->a = a - 1;
      }
      tolk_assert(apply_op(t, *oq_[q_]));
      ++q_;
      ++pos;
    }
  }
  tolk_assert(!pos);
  t.apply_push_newconst();
  tolk_assert(t <= tr_[p_ - 1]);
  oq_[q_++] = std::move(c_op);
  show_right();
  return true;
}

bool Optimizer::rewrite(int p, AsmOp&& new_op) {
  tolk_assert(p > 0 && p <= l_);
  p_ = p;
  q_ = 1;
  show_left();
  oq_[0] = std::move(op_[0]);
  *oq_[0] = new_op;
  show_right();
  return true;
}

bool Optimizer::rewrite(int p, AsmOp&& new_op1, AsmOp&& new_op2) {
  tolk_assert(p > 1 && p <= l_);
  p_ = p;
  q_ = 2;
  show_left();
  oq_[0] = std::move(op_[0]);
  *oq_[0] = new_op1;
  oq_[1] = std::move(op_[1]);
  *oq_[1] = new_op2;
  show_right();
  return true;
}

bool Optimizer::rewrite(int p, AsmOp&& new_op1, AsmOp&& new_op2, AsmOp&& new_op3) {
  tolk_assert(p > 2 && p <= l_);
  p_ = p;
  q_ = 3;
  show_left();
  oq_[0] = std::move(op_[0]);
  *oq_[0] = new_op1;
  oq_[1] = std::move(op_[1]);
  *oq_[1] = new_op2;
  oq_[2] = std::move(op_[2]);
  *oq_[2] = new_op3;
  show_right();
  return true;
}

bool Optimizer::rewrite_nop() {
  tolk_assert(p_ > 0 && p_ <= l_);
  q_ = 0;
  show_left();
  show_right();
  return true;
}

bool Optimizer::is_pred(const std::function<bool(const StackTransform&)>& pred, int min_p) {
  min_p = std::max(min_p, pb_);
  for (int p = l2_; p >= min_p; p--) {
    if (pred(tr_[p - 1])) {
      p_ = p;
      return true;
    }
  }
  return false;
}

bool Optimizer::is_same_as(const StackTransform& trans, int min_p) {
  return is_pred([&trans](const auto& t) { return t >= trans; }, min_p);
}

// s1 s3 XCHG ; s0 s2 XCHG -> 2SWAP
bool Optimizer::is_2swap() {
  static const StackTransform t_2swap{2, 3, 0, 1, 4};
  return is_same_as(t_2swap);
}

// s3 PUSH ; s3 PUSH -> 2OVER
bool Optimizer::is_2over() {
  static const StackTransform t_2over{2, 3, 0};
  return is_same_as(t_2over);
}

bool Optimizer::is_2dup() {
  static const StackTransform t_2dup{0, 1, 0};
  return is_same_as(t_2dup);
}

bool Optimizer::is_tuck() {
  static const StackTransform t_tuck{0, 1, 0, 2};
  return is_same_as(t_tuck);
}

bool Optimizer::is_2drop() {
  static const StackTransform t_2drop{2};
  return is_same_as(t_2drop);
}

bool Optimizer::is_rot() {
  return is_pred([](const auto& t) { return t.is_rot(); });
}

bool Optimizer::is_rotrev() {
  return is_pred([](const auto& t) { return t.is_rotrev(); });
}

bool Optimizer::is_nop() {
  return is_pred([](const auto& t) { return t.is_id(); }, 1);
}

bool Optimizer::is_xchg(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_xchg(i, j) && ((*i < 16 && *j < 16) || (!*i && *j < 256)); });
}

bool Optimizer::is_xchg_xchg(int* i, int* j, int* k, int* l) {
  return is_pred([i, j, k, l](const auto& t) {
           return t.is_xchg_xchg(i, j, k, l) && (*i < 2 && *j < (*i ? 16 : 256) && *k < 2 && *l < (*k ? 16 : 256));
         }) &&
         (!(p_ == 2 && op_[0]->is_xchg(*i, *j) && op_[1]->is_xchg(*k, *l)));
}

bool Optimizer::is_push(int* i) {
  return is_pred([i](const auto& t) { return t.is_push(i) && *i < 256; });
}

bool Optimizer::is_pop(int* i) {
  return is_pred([i](const auto& t) { return t.is_pop(i) && *i < 256; });
}

bool Optimizer::is_pop_pop(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_pop_pop(i, j) && *i < 256 && *j < 256; }, 3);
}

bool Optimizer::is_push_rot(int* i) {
  return is_pred([i](const auto& t) { return t.is_push_rot(i) && *i < 16; }, 3);
}

bool Optimizer::is_push_rotrev(int* i) {
  return is_pred([i](const auto& t) { return t.is_push_rotrev(i) && *i < 16; }, 3);
}

bool Optimizer::is_push_xchg(int* i, int* j, int* k) {
  return is_pred([i, j, k](const auto& t) { return t.is_push_xchg(i, j, k) && *i < 16 && *j < 16 && *k < 16; }) &&
         !(p_ == 2 && op_[0]->is_push() && op_[1]->is_xchg());
}

bool Optimizer::is_xchg2(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_xchg2(i, j) && *i < 16 && *j < 16; });
}

bool Optimizer::is_xcpu(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_xcpu(i, j) && *i < 16 && *j < 16; });
}

bool Optimizer::is_puxc(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_puxc(i, j) && *i < 16 && *j < 15; });
}

bool Optimizer::is_push2(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_push2(i, j) && *i < 16 && *j < 16; });
}

bool Optimizer::is_xchg3(int* i, int* j, int* k) {
  return is_pred([i, j, k](const auto& t) { return t.is_xchg3(i, j, k) && *i < 16 && *j < 16 && *k < 16; });
}

bool Optimizer::is_xc2pu(int* i, int* j, int* k) {
  return is_pred([i, j, k](const auto& t) { return t.is_xc2pu(i, j, k) && *i < 16 && *j < 16 && *k < 16; });
}

bool Optimizer::is_xcpuxc(int* i, int* j, int* k) {
  return is_pred([i, j, k](const auto& t) { return t.is_xcpuxc(i, j, k) && *i < 16 && *j < 16 && *k < 15; });
}

bool Optimizer::is_xcpu2(int* i, int* j, int* k) {
  return is_pred([i, j, k](const auto& t) { return t.is_xcpu2(i, j, k) && *i < 16 && *j < 16 && *k < 16; });
}

bool Optimizer::is_puxc2(int* i, int* j, int* k) {
  return is_pred(
      [i, j, k](const auto& t) { return t.is_puxc2(i, j, k) && *i < 16 && *j < 15 && *k < 15 && *j + *k != -1; });
}

bool Optimizer::is_puxcpu(int* i, int* j, int* k) {
  return is_pred([i, j, k](const auto& t) { return t.is_puxcpu(i, j, k) && *i < 16 && *j < 15 && *k < 15; });
}

bool Optimizer::is_pu2xc(int* i, int* j, int* k) {
  return is_pred([i, j, k](const auto& t) { return t.is_pu2xc(i, j, k) && *i < 16 && *j < 15 && *k < 14; });
}

bool Optimizer::is_push3(int* i, int* j, int* k) {
  return is_pred([i, j, k](const auto& t) { return t.is_push3(i, j, k) && *i < 16 && *j < 16 && *k < 16; });
}

bool Optimizer::is_blkswap(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_blkswap(i, j) && *i > 0 && *j > 0 && *i <= 16 && *j <= 16; });
}

bool Optimizer::is_blkpush(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_blkpush(i, j) && *i > 0 && *i < 16 && *j < 16; });
}

bool Optimizer::is_blkdrop(int* i) {
  return is_pred([i](const auto& t) { return t.is_blkdrop(i) && *i > 0 && *i < 16; });
}

bool Optimizer::is_blkdrop2(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_blkdrop2(i, j) && *i > 0 && *i < 16 && *j > 0 && *j < 16; });
}

bool Optimizer::is_reverse(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_reverse(i, j) && *i >= 2 && *i <= 17 && *j < 16; });
}

bool Optimizer::is_nip_seq(int* i, int* j) {
  return is_pred([i, j](const auto& t) { return t.is_nip_seq(i, j) && *i >= 3 && *i <= 15; });
}

bool Optimizer::is_pop_blkdrop(int* i, int* k) {
  return is_pred([i, k](const auto& t) { return t.is_pop_blkdrop(i, k) && *i >= *k && *k >= 2 && *k <= 15; }, 3);
}

bool Optimizer::is_2pop_blkdrop(int* i, int* j, int* k) {
  return is_pred(
      [i, j, k](const auto& t) { return t.is_2pop_blkdrop(i, j, k) && *i >= *k && *j >= *k && *k >= 2 && *k <= 15; },
      3);
}

bool Optimizer::compute_stack_transforms() {
  StackTransform trans;
  for (int i = 0; i < l_; i++) {
    if (!apply_op(trans, *op_[i])) {
      l2_ = i;
      return true;
    }
    tr_[i] = trans;
  }
  l2_ = l_;
  return true;
}

bool Optimizer::show_stack_transforms() const {
  show_head();
  // slow version
  /*
  StackTransform trans2;
  std::cerr << "id = " << trans2 << std::endl;
  for (int i = 0; i < l_; i++) {
    StackTransform op;
    if (!apply_op(op, *op_[i])) {
      std::cerr << "* (" << *op_[i] << " = invalid)\n";
      break;
    }
    trans2 *= op;
    std::cerr << "* " << *op_[i] << " = " << op << " -> " << trans2 << std::endl;
  }
  */
  // fast version
  StackTransform trans;
  for (int i = 0; i < l_; i++) {
    std::cerr << trans << std::endl << *op_[i] << " -> ";
    if (!apply_op(trans, *op_[i])) {
      std::cerr << " <not-applicable>" << std::endl;
      return true;
    }
  }
  std::cerr << trans << std::endl;
  return true;
}

bool Optimizer::find_at_least(int pb) {
  p_ = q_ = 0;
  pb_ = pb;
  // show_stack_transforms();
  int i, j, k, l, c;
  SrcLocation loc;      // for asm ops inserted by optimizer, leave location empty (in fift output, it'll be attached to above)
  return (is_push_const(&i, &c) && rewrite_push_const(i, c)) || (is_nop() && rewrite_nop()) ||
         (!(mode_ & 1) && is_const_rot(&c) && rewrite_const_rot(c)) ||
         (is_const_push_xchgs() && rewrite_const_push_xchgs()) || (is_const_pop(&c, &i) && rewrite_const_pop(c, i)) ||
         (is_xchg(&i, &j) && rewrite(AsmOp::Xchg(loc, i, j))) || (is_push(&i) && rewrite(AsmOp::Push(loc, i))) ||
         (is_pop(&i) && rewrite(AsmOp::Pop(loc, i))) || (is_pop_pop(&i, &j) && rewrite(AsmOp::Pop(loc, i), AsmOp::Pop(loc, j))) ||
         (is_xchg_xchg(&i, &j, &k, &l) && rewrite(AsmOp::Xchg(loc, i, j), AsmOp::Xchg(loc, k, l))) ||
         detect_rewrite_big_THROW() ||
         detect_rewrite_MY_store_int() || detect_rewrite_MY_skip_bits() || detect_rewrite_NEWC_PUSH_STUR() ||
         detect_rewrite_LDxx_DROP() ||
         detect_rewrite_SWAP_symmetric() || detect_rewrite_SWAP_PUSH_STUR() || detect_rewrite_SWAP_STxxxR() ||
         detect_rewrite_NOT_THROWIF() ||
         (!(mode_ & 1) &&
          ((is_rot() && rewrite(AsmOp::Custom(loc, "ROT", 3, 3))) || (is_rotrev() && rewrite(AsmOp::Custom(loc, "-ROT", 3, 3))) ||
           (is_2dup() && rewrite(AsmOp::Custom(loc, "2DUP", 2, 4))) ||
           (is_2swap() && rewrite(AsmOp::Custom(loc, "2SWAP", 2, 4))) ||
           (is_2over() && rewrite(AsmOp::Custom(loc, "2OVER", 2, 4))) ||
           (is_tuck() && rewrite(AsmOp::Custom(loc, "TUCK", 2, 3))) ||
           (is_2drop() && rewrite(AsmOp::Custom(loc, "2DROP", 2, 0))) || (is_xchg2(&i, &j) && rewrite(AsmOp::Xchg2(loc, i, j))) ||
           (is_xcpu(&i, &j) && rewrite(AsmOp::XcPu(loc, i, j))) || (is_puxc(&i, &j) && rewrite(AsmOp::PuXc(loc, i, j))) ||
           (is_push2(&i, &j) && rewrite(AsmOp::Push2(loc, i, j))) || (is_blkswap(&i, &j) && rewrite(AsmOp::BlkSwap(loc, i, j))) ||
           (is_blkpush(&i, &j) && rewrite(AsmOp::BlkPush(loc, i, j))) || (is_blkdrop(&i) && rewrite(AsmOp::BlkDrop(loc, i))) ||
           (is_push_rot(&i) && rewrite(AsmOp::Push(loc, i), AsmOp::Custom(loc, "ROT"))) ||
           (is_push_rotrev(&i) && rewrite(AsmOp::Push(loc, i), AsmOp::Custom(loc, "-ROT"))) ||
           (is_push_xchg(&i, &j, &k) && rewrite(AsmOp::Push(loc, i), AsmOp::Xchg(loc, j, k))) ||
           (is_reverse(&i, &j) && rewrite(AsmOp::BlkReverse(loc, i, j))) ||
           (is_blkdrop2(&i, &j) && rewrite(AsmOp::BlkDrop2(loc, i, j))) ||
           (is_nip_seq(&i, &j) && rewrite(AsmOp::Xchg(loc, i, j), AsmOp::BlkDrop(loc, i))) ||
           (is_pop_blkdrop(&i, &k) && rewrite(AsmOp::Pop(loc, i), AsmOp::BlkDrop(loc, k))) ||
           (is_2pop_blkdrop(&i, &j, &k) && (k >= 3 && k <= 13 && i != j + 1 && i <= 15 && j <= 14
                                                ? rewrite(AsmOp::Xchg2(loc, j + 1, i), AsmOp::BlkDrop(loc, k + 2))
                                                : rewrite(AsmOp::Pop(loc, i), AsmOp::Pop(loc, j), AsmOp::BlkDrop(loc, k)))) ||
           (is_xchg3(&i, &j, &k) && rewrite(AsmOp::Xchg3(loc, i, j, k))) ||
           (is_xc2pu(&i, &j, &k) && rewrite(AsmOp::Xc2Pu(loc, i, j, k))) ||
           (is_xcpuxc(&i, &j, &k) && rewrite(AsmOp::XcPuXc(loc, i, j, k))) ||
           (is_xcpu2(&i, &j, &k) && rewrite(AsmOp::XcPu2(loc, i, j, k))) ||
           (is_puxc2(&i, &j, &k) && rewrite(AsmOp::PuXc2(loc, i, j, k))) ||
           (is_puxcpu(&i, &j, &k) && rewrite(AsmOp::PuXcPu(loc, i, j, k))) ||
           (is_pu2xc(&i, &j, &k) && rewrite(AsmOp::Pu2Xc(loc, i, j, k))) ||
           (is_push3(&i, &j, &k) && rewrite(AsmOp::Push3(loc, i, j, k)))));
}

bool Optimizer::find() {
  if (!compute_stack_transforms()) {
    return false;
  }
  for (int pb = l_; pb > 0; --pb) {
    if (find_at_least(pb)) {
      return true;
    }
  }
  return false;
}

bool Optimizer::optimize() {
  bool f = false;
  while (find()) {
    f = true;
    apply();
    unpack();
  }
  return f;
}

AsmOpConsList optimize_code_head(AsmOpConsList op_list, int mode) {
  Optimizer opt(std::move(op_list), false, mode);
  opt.optimize();
  return opt.extract_code();
}

AsmOpConsList optimize_code(AsmOpConsList op_list, int mode) {
  std::vector<std::unique_ptr<AsmOp>> v;
  while (op_list) {
    if (!op_list->car->is_comment()) {
      op_list = optimize_code_head(std::move(op_list), mode);
    }
    if (op_list) {
      v.push_back(std::move(op_list->car));
      op_list = std::move(op_list->cdr);
    }
  }
  for (auto it = v.rbegin(); it < v.rend(); ++it) {
    op_list = AsmOpCons::cons(std::move(*it), std::move(op_list));
  }
  return std::move(op_list);
}

void optimize_code(AsmOpList& ops) {
  AsmOpConsList op_list;
  for (auto it = ops.list_.rbegin(); it < ops.list_.rend(); ++it) {
    op_list = AsmOpCons::cons(std::make_unique<AsmOp>(std::move(*it)), std::move(op_list));
  }
  for (int mode : {1, 1, 1, 1, 0, 0, 0, 0}) {
    op_list = optimize_code(std::move(op_list), mode);
  }
  ops.list_.clear();
  while (op_list) {
    ops.list_.push_back(std::move(*(op_list->car)));
    op_list = std::move(op_list->cdr);
  }
}

}  // namespace tolk
