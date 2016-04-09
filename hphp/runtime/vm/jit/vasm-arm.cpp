/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/vasm-emit.h"

#include "hphp/runtime/vm/jit/abi-arm.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/mc-generator.h"
#include "hphp/runtime/vm/jit/print.h"
#include "hphp/runtime/vm/jit/reg-algorithms.h"
#include "hphp/runtime/vm/jit/service-requests.h"
#include "hphp/runtime/vm/jit/smashable-instr-arm.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-internal.h"
#include "hphp/runtime/vm/jit/vasm-lower.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-util.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include "hphp/vixl/a64/macro-assembler-a64.h"

TRACE_SET_MOD(vasm);

namespace HPHP { namespace jit {
///////////////////////////////////////////////////////////////////////////////

using namespace arm;
using namespace vixl;

namespace arm { struct ImmFolder; }

namespace {
///////////////////////////////////////////////////////////////////////////////

vixl::Register X(Vreg64 r) {
  PhysReg pr(r.asReg());
  return x2a(pr);
}

vixl::Register W(Vreg64 r) {
  PhysReg pr(r.asReg());
  return x2a(pr).W();
}

vixl::Register W(Vreg32 r) {
  PhysReg pr(r.asReg());
  return x2a(pr).W();
}

vixl::Register W(Vreg16 r) {
  PhysReg pr(r.asReg());
  return x2a(pr).W();
}

vixl::Register W(Vreg8 r) {
  PhysReg pr(r.asReg());
  return x2a(pr).W();
}

vixl::FPRegister D(Vreg r) {
  return x2f(r);
}

vixl::VRegister V(Vreg r) {
  return x2v(r);
}

vixl::MemOperand M(Vptr p) {
  assertx(p.base.isValid());
  if (p.index.isValid()) {
    assertx(p.disp == 0);
    return X(p.base)[p.index];
  }
  assertx(p.disp >= -256 && p.disp <= 255);
  return X(p.base)[p.disp];
}

vixl::Condition C(ConditionCode cc) {
  return arm::convertCC(cc);
}

///////////////////////////////////////////////////////////////////////////////

struct Vgen {
  explicit Vgen(Venv& env)
    : env(env)
    , assem(*env.cb)
    , a(&assem)
    , current(env.current)
    , next(env.next)
    , jmps(env.jmps)
    , jccs(env.jccs)
    , catches(env.catches)
  {}

  static void patch(Venv& env);
  static void pad(CodeBlock& cb) {}

  /////////////////////////////////////////////////////////////////////////////

  template<class Inst> void emit(Inst& i) {
    always_assert_flog(false, "unimplemented instruction: {} in B{}\n",
                       vinst_names[Vinstr(i).op], size_t(current));
  }

  // intrinsics
  void emit(const copy& i);
  void emit(const copy2& i);
  void emit(const debugtrap& i) { a->Brk(0); }
  void emit(const fallthru& i) {}
  void emit(const ldimmb& i);
  void emit(const ldimml& i);
  void emit(const ldimmq& i);
  void emit(const ldimmqs& i);
  void emit(const ldimmw& i);
  void emit(const load& i);
  void emit(const store& i);
  void emit(const mcprep& i);

  // native function abi
  void emit(const call& i);
  void emit(const callm& i);
  void emit(const callr& i) { a->Blr(X(i.target)); }
  void emit(const calls& i);
  void emit(const ret& i) { a->Ret(); }

  // stub function abi
  void emit(const stublogue& i);
  void emit(const stubret& i);
  void emit(const callstub& i);
  void emit(const callfaststub& i);
  void emit(const tailcallstub& i);

  // php function abi
  void emit(const phplogue& i);
  void emit(const phpret& i);
  void emit(const callphp& i);
  void emit(const tailcallphp& i);
  void emit(const callarray& i);
  void emit(const contenter& i);

  // vm entry abi
  void emit(const calltc&);
  void emit(const leavetc&) { a->Ret(); }

  // exceptions
  void emit(const landingpad& i) {}
  void emit(const nothrow& i);
  void emit(const syncpoint& i);
  void emit(const unwind& i);

  // instructions
  void emit(const absdbl& i) { a->Fabs(D(i.d), D(i.s)); }
  void emit(const addl& i) { a->Add(W(i.d), W(i.s1), W(i.s0), SetFlags); }
  void emit(const addli& i) { a->Add(W(i.d), W(i.s1), i.s0.l(), SetFlags); }
  void emit(const addq& i) { a->Add(X(i.d), X(i.s1), X(i.s0), SetFlags); }
  void emit(const addqi& i) { a->Add(X(i.d), X(i.s1), i.s0.q(), SetFlags); }
  void emit(const addsd& i) { a->Fadd(D(i.d), D(i.s1), D(i.s0)); }
  void emit(const andb& i) { a->And(W(i.d), W(i.s1), W(i.s0), SetFlags); }
  void emit(const andbi& i) { a->And(W(i.d), W(i.s1), i.s0.l(), SetFlags); };
  void emit(const andl& i) { a->And(W(i.d), W(i.s1), W(i.s0), SetFlags); }
  void emit(const andli& i) { a->And(W(i.d), W(i.s1), i.s0.l(), SetFlags); }
  void emit(const andq& i) { a->And(X(i.d), X(i.s1), X(i.s0), SetFlags); }
  void emit(const andqi& i) { a->And(X(i.d), X(i.s1), i.s0.q(), SetFlags); }
  void emit(const cloadq& i);
  void emit(const cmovq& i) { a->Csel(X(i.d), X(i.t), X(i.f), C(i.cc)); }
  void emit(const cmpl& i) { a->Cmp(W(i.s1), W(i.s0)); }
  void emit(const cmpli& i) { a->Cmp(W(i.s1), i.s0.l()); }
  void emit(const cmpq& i) { a->Cmp(X(i.s1), X(i.s0)); }
  void emit(const cmpqi& i) { a->Cmp(X(i.s1), i.s0.q()); }
  void emit(const cmpsd& i);
  void emit(const cvtsi2sd& i) { a->Scvtf(D(i.d), X(i.s)); }
  void emit(const cvttsd2siq& i) { a->Fcvtzs(X(i.d), D(i.s)); }
  void emit(const decl& i) { a->Sub(W(i.d), W(i.s), 1, SetFlags); }
  void emit(const decq& i) { a->Sub(X(i.d), X(i.s), 1, SetFlags); }
  void emit(const divint& i) { a->Sdiv(X(i.d), X(i.s0), X(i.s1)); }
  void emit(const divsd& i) { a->Fdiv(D(i.d), D(i.s1), D(i.s0)); }
  void emit(const imul& i);
  void emit(const incl& i) { a->Add(W(i.d), W(i.s), 1, SetFlags); }
  void emit(const incq& i) { a->Add(X(i.d), X(i.s), 1, SetFlags); }
  void emit(const incqmlock& i);
  void emit(const incw& i) { a->Add(W(i.d), W(i.s), 1, SetFlags); }
  void emit(const jcc& i);
  void emit(const jcci& i);
  void emit(const jmp& i);
  void emit(const jmpi& i);
  void emit(const jmpm& i);
  void emit(const jmpr& i) { a->Br(X(i.target)); }
  void emit(const lea& i);
  void emit(const leap& i) { a->Mov(X(i.d), i.s.r.disp); }
  void emit(const loadb& i) { a->Ldrsb(W(i.d), M(i.s)); }
  void emit(const loadl& i) { a->Ldr(W(i.d), M(i.s)); }
  void emit(const loadqp& i);
  void emit(const loadsd& i) { a->Ldr(D(i.d), M(i.s)); }
  void emit(const loadtqb& i) { a->Ldrsb(W(i.d), M(i.s)); }
  void emit(const loadups& i);
  void emit(const loadw& i) { a->Ldrsh(W(i.d), M(i.s)); }
  void emit(const loadzbl& i) { a->Ldrb(W(i.d), M(i.s)); }
  void emit(const loadzbq& i) { a->Ldrb(W(i.d), M(i.s)); }
  void emit(const loadzlq& i) { a->Ldr(W(i.d), M(i.s)); }
  void emit(const movb& i) { a->Mov(W(i.d), W(i.s)); }
  void emit(const movl& i) { a->Mov(W(i.d), W(i.s)); }
  void emit(const movtqb& i) { a->Mov(W(i.d), W(i.s)); }
  void emit(const movtql& i) { a->Mov(W(i.d), W(i.s)); }
  void emit(const movzbl& i) { a->Uxtb(W(i.d), W(i.s)); }
  void emit(const movzbq& i) { a->Uxtb(X(i.d), W(i.s).X()); }
  void emit(const mulsd& i) { a->Fmul(D(i.d), D(i.s1), D(i.s0)); }
  void emit(const neg& i) { a->Neg(X(i.d), X(i.s), SetFlags); }
  void emit(const nop& i) { a->Nop(); }
  void emit(const notb& i) { a->Mvn(W(i.d), W(i.s)); }
  void emit(const not& i) { a->Mvn(X(i.d), X(i.s)); }
  void emit(const orq& i);
  void emit(const orqi& i);
  void emit(const pop& i) { a->Ldr(X(i.d), MemOperand(sp, 8, PostIndex)); }
  void emit(const popm& i);
  void emit(const psllq& i);
  void emit(const psrlq& i);
  void emit(const push& i) { a->Str(X(i.s), MemOperand(sp, -8, PreIndex)); }
  void emit(const pushm& i);
  void emit(const roundsd& i);
  void emit(const sar& i);
  void emit(const sarqi& i);
  void emit(const setcc& i) { a->Cset(X(PhysReg(i.d.asReg())), C(i.cc)); }
  void emit(const shl& i);
  void emit(const shlli& i);
  void emit(const shlqi& i);
  void emit(const shrli& i);
  void emit(const shrqi& i);
  void emit(const sqrtsd& i) { a->Fsqrt(D(i.d), D(i.s)); }
  void emit(const srem& i);
  void emit(const storeb& i) { a->Strb(W(i.s), M(i.m)); }
  void emit(const storebi& i);
  void emit(const storel& i) { a->Str(W(i.s), M(i.m)); }
  void emit(const storeli& i);
  void emit(const storeqi& i);
  void emit(const storesd& i);
  void emit(const storeups& i);
  void emit(const storew& i) { a->Strh(W(i.s), M(i.m)); }
  void emit(const storewi& i);
  void emit(const subbi& i) { a->Sub(W(i.d), W(i.s1), i.s0.l(), SetFlags); }
  void emit(const subl& i) { a->Sub(W(i.d), W(i.s1), W(i.s0), SetFlags); }
  void emit(const subli& i) { a->Sub(W(i.d), W(i.s1), i.s0.l(), SetFlags); }
  void emit(const subq& i) { a->Sub(X(i.d), X(i.s1), X(i.s0), SetFlags); }
  void emit(const subqi& i) { a->Sub(X(i.d), X(i.s1), i.s0.q(), SetFlags); }
  void emit(const subsd& i) { a->Fsub(D(i.d), D(i.s1), D(i.s0)); }
  void emit(const testb& i) { a->Tst(W(i.s1), W(i.s0)); }
  void emit(const testbi& i) { a->Tst(W(i.s1), i.s0.l()); }
  void emit(const testl& i) { a->Tst(W(i.s1), W(i.s0)); }
  void emit(const testli& i) { a->Tst(W(i.s1), i.s0.l()); }
  void emit(const testq& i) { a->Tst(X(i.s1), X(i.s0)); }
  void emit(const testqi& i) { a->Tst(X(i.s1), i.s0.q()); }
  void emit(const ucomisd& i) { a->Fcmp(D(i.s0), D(i.s1)); }
  void emit(const ud2& i) { a->Brk(1); }
  void emit(const unpcklpd&);
  void emit(const xorb& i);
  void emit(const xorbi& i);
  void emit(const xorl& i);
  void emit(const xorq& i);
  void emit(const xorqi& i);

  // arm intrinsics
  void emit(const addqinf& i) { a->Add(X(i.d), X(i.s1), i.s0.q()); }
  void emit(const mrs& i) { a->Mrs(X(i.r), vixl::SystemRegister(i.s.l())); }
  void emit(const msr& i) { a->Msr(vixl::SystemRegister(i.s.l()), X(i.r)); }
  void emit(const orli& i);
  void emit(const pushp& i);
  void emit(const shlqinf& i) { a->Lsl(X(i.d), X(i.s1), i.s0.l()); }
private:
  CodeBlock& frozen() { return env.text.frozen().code; }

private:
  Venv& env;
  vixl::MacroAssembler assem;
  vixl::MacroAssembler* a;

  const Vlabel current;
  const Vlabel next;
  jit::vector<Venv::LabelPatch>& jmps;
  jit::vector<Venv::LabelPatch>& jccs;
  jit::vector<Venv::LabelPatch>& catches;
};

///////////////////////////////////////////////////////////////////////////////

void Vgen::patch(Venv& env) {
  for (auto& p : env.jmps) {
    assertx(env.addrs[p.target]);
    // 'jmp' is 2 instructions, load followed by branch
    *reinterpret_cast<TCA*>(p.instr + 2 * 4) = env.addrs[p.target];
  }
  for (auto& p : env.jccs) {
    assertx(env.addrs[p.target]);
    // 'jcc' is 3 instructions, b.!cc + load followed by branch
    *reinterpret_cast<TCA*>(p.instr + 3 * 4) = env.addrs[p.target];
  }
  assertx(env.bccs.empty());
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const copy& i) {
  if (i.s.isGP() && i.d.isGP()) {
    a->Mov(X(i.d), X(i.s));
  } else if (i.s.isSIMD() && i.d.isGP()) {
    a->Fmov(X(i.d), D(i.s));
  } else if (i.s.isGP() && i.d.isSIMD()) {
    a->Fmov(D(i.d), X(i.s));
  } else {
    assertx(i.s.isSIMD() && i.d.isSIMD());
    a->Fmov(D(i.d), D(i.s));
  }
}

void Vgen::emit(const copy2& i) {
  MovePlan moves;
  Reg64 d0 = i.d0, d1 = i.d1, s0 = i.s0, s1 = i.s1;
  moves[d0] = s0;
  moves[d1] = s1;
  auto howTo = doRegMoves(moves, rAsm); // rAsm isn't used.
  for (auto& how : howTo) {
    if (how.m_kind == MoveInfo::Kind::Move) {
      a->Mov(X(how.m_dst), X(how.m_src));
    } else {
      auto const d = X(how.m_dst);
      auto const s = X(how.m_src);
      a->Eor(d, d, s);
      a->Eor(s, d, s);
      a->Eor(d, d, s);
    }
  }
}

void emitSimdImmInt(vixl::MacroAssembler* a, int64_t val, Vreg d) {
  // Assembler::fmov emits a ldr from a literal pool if IsImmFP64 is false.
  // In that case, emit the raw bits into a GPR first and then move them
  // unmodified into destination SIMD
  union { double dval; int64_t ival; };
  ival = val;
  if (vixl::Assembler::IsImmFP64(dval)) {
    a->Fmov(D(d), dval);
  } else if (ival == 0) {
    a->Fmov(D(d), vixl::xzr);
  } else {
    a->Mov(rAsm, ival);
    a->Fmov(D(d), rAsm);
  }
}

#define Y(vasm_opc, simd_w, vr, gpr_w, imm_w) \
void Vgen::emit(const vasm_opc& i) {          \
  if (i.d.isSIMD()) {                         \
    emitSimdImmInt(a, i.s.simd_w(), i.d);     \
  } else {                                    \
    vr d = i.d;                               \
    a->Mov(gpr_w(d), i.s.imm_w());            \
  }                                           \
}

Y(ldimmb, ub, Vreg8, W, l)
Y(ldimmw, w, Vreg16, W, l)
Y(ldimml, l, Vreg32, W, l)
Y(ldimmq, q, Vreg64, X, q)

#undef Y

void Vgen::emit(const ldimmqs& i) {
  emitSmashableMovq(a->code(), env.meta, i.s.q(), i.d);
}

void Vgen::emit(const load& i) {
  if (i.d.isGP()) {
    a->Ldr(X(i.d), M(i.s));
  } else {
    a->Ldr(D(i.d), M(i.s));
  }
}

void Vgen::emit(const store& i) {
  if (i.s.isGP()) {
    a->Str(X(i.s), M(i.d));
  } else {
    a->Str(D(i.s), M(i.d));
  }
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const mcprep& i) {
  /*
   * Initially, we set the cache to hold (addr << 1) | 1 (where `addr' is the
   * address of the movq) so that we can find the movq from the handler.
   *
   * We set the low bit for two reasons: the Class* will never be a valid
   * Class*, so we'll always miss the inline check before it's smashed, and
   * handlePrimeCacheInit can tell it's not been smashed yet
   */
  auto const mov_addr = emitSmashableMovq(a->code(), env.meta, 0, r64(i.d));
  auto const imm = reinterpret_cast<uint64_t>(mov_addr);
  smashMovq(mov_addr, (imm << 1) | 1);

  env.meta.addressImmediates.insert(reinterpret_cast<TCA>(~imm));
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const call& i) {
  a->Mov(rAsm, reinterpret_cast<uint64_t>(i.target));
  a->Blr(rAsm);
}

void Vgen::emit(const callm& i) {
  a->Ldr(rAsm, M(i.target));
  a->Blr(rAsm);
}

void Vgen::emit(const calls& i) {
  emitSmashableCall(a->code(), env.meta, i.target);
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const stublogue& i) {
  // Push FP, LR always regardless of i.saveframe (makes SP 16B aligned)
  emit(pushp{rfp(), rlink()});
}

void Vgen::emit(const stubret& i) {
  if(i.saveframe) {
    a->Ldp(X(rfp()), X(rlink()), MemOperand(sp, 16, PostIndex));
  } else {
    a->Ldp(rAsm, X(rlink()), MemOperand(sp, 16, PostIndex));
  }
  a->Ret();
}

void Vgen::emit(const callstub& i) {
  emit(call{i.target, i.args});
}

void Vgen::emit(const callfaststub& i) {
  emit(call{i.target, i.args});
  emit(syncpoint{i.fix});
}

void Vgen::emit(const tailcallstub& i) {
  // SP is 16B aligned here, just jmp
  emit(jmpi{i.target, i.args});
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const phplogue& i) {
  // Save LR in m_savedRip on the current VM frame pointed by 'i.fp'
  a->Str(X(rlink()), X(i.fp)[AROFF(m_savedRip)]);
}

void Vgen::emit(const phpret& i) {
  a->Ldr(X(rlink()), X(i.fp)[AROFF(m_savedRip)]);
  if (!i.noframe) {
    a->Ldr(X(i.d), X(i.fp)[AROFF(m_sfp)]);
  }
  a->Ret();
}

void Vgen::emit(const callphp& i) {
  emitSmashableCall(a->code(), env.meta, i.stub);
  emit(unwind{{i.targets[0], i.targets[1]}});
}

void Vgen::emit(const tailcallphp& i) {
  // To make callee's return as caller's return, load the return address at
  // i.fp[AROFF(m_savedRip)] into LR and jmp to target
  a->Ldr(X(rlink()), X(i.fp)[AROFF(m_savedRip)]);
  emit(jmpr{i.target, i.args});
}

void Vgen::emit(const callarray& i) {
  emit(call{i.target, i.args});
}

void Vgen::emit(const contenter& i) {
  vixl::Label Stub, End;

  a->B(&End);
  a->bind(&Stub);

  a->Ldr(rAsm, sp[0]);
  a->Str(rAsm, X(i.fp)[AROFF(m_savedRip)]);
  a->Br(X(i.target));

  a->bind(&End);
  a->Ldr(rAsm, &Stub);
  a->Blr(rAsm);
  // m_savedRip will point here.
  emit(unwind{{i.targets[0], i.targets[1]}});
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const calltc& i) {
  vixl::Label stub;

  // Just call next instruction to balance branch predictor's call return stack.
  // Rest is implemented in lower(..)
  a->Bl(&stub);
  a->bind(&stub);
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const nothrow& i) {
  env.meta.catches.emplace_back(a->frontier(), nullptr);
}

void Vgen::emit(const syncpoint& i) {
  FTRACE(5, "IR recordSyncPoint: {} {} {}\n", a->frontier(),
         i.fix.pcOffset, i.fix.spOffset);
  env.meta.fixups.emplace_back(a->frontier(), i.fix);
}

void Vgen::emit(const unwind& i) {
  catches.push_back({a->frontier(), i.targets[1]});
  emit(jmp{i.targets[0]});
}

///////////////////////////////////////////////////////////////////////////////

void Vgen::emit(const cloadq& i) {
  a->Ldr(rAsm, M(i.t));
  a->Csel(X(i.d), rAsm, X(i.f), C(i.cc));
}

void Vgen::emit(const cmpsd& i) {
  // lower(...) saves and restores the flags register. So, 'Fcmp' can be used
  a->Fcmp(D(i.s0), D(i.s1));
  switch(i.pred) {
    case ComparisonPred::eq_ord: {
      a->Csetm(rAsm, C(jit::CC_E));
      break;
    }

    case ComparisonPred::ne_unord: {
      a->Csetm(rAsm, C(jit::CC_NE));
      break;
    }

    default: {
      always_assert(false);
    }
  }
  a->Fmov(D(i.d), rAsm);
}

/*
 * Flags
 *   SF should be set to MSB of the result
 *   CF, OF should be set to (1, 1) if the result is truncated, (0, 0) otherwise
 *   ZF, AF, PF are undefined
 *
 * In the following implementation,
 *   N, Z are updated according to result
 *   C, V are cleared (FIXME)
 *   PF, AF are not available
 */
void Vgen::emit(const imul& i) {
  a->Mul(X(i.d), X(i.s0), X(i.s1));
  a->Bic(vixl::xzr, X(i.d), vixl::xzr, SetFlags);
}

void Vgen::emit(const incqmlock& i) {
  auto adr = M(i.m);
  vixl::Label again;
  a->bind(&again);
  a->ldxr(rAsm, adr);
  a->Add(rAsm, rAsm, 1, SetFlags);
  a->stxr(rAsm.W(), rAsm, adr);
  a->Cbnz(rAsm.W(), &again);
}

void Vgen::emit(const jcc& i) {
  if (i.targets[1] != i.targets[0]) {
    if (next == i.targets[1]) {
      return emit(jcc{ccNegate(i.cc), i.sf, {i.targets[1], i.targets[0]}});
    }
    auto taken = i.targets[1];
    jccs.push_back({a->frontier(), taken});
    vixl::Label skip, data;

    a->B(&skip, vixl::InvertCondition(C(i.cc)));
    a->Ldr(rAsm, &data);
    a->Br(rAsm);
    a->bind(&data);
    a->dc64(reinterpret_cast<int64_t>(a->frontier()));
    a->bind(&skip);
  }
  emit(jmp{i.targets[0]});
}

void Vgen::emit(const jcci& i) {
  vixl::Label skip, data;

  a->B(&skip, vixl::InvertCondition(C(i.cc)));
  a->Ldr(rAsm, &data);
  a->Br(rAsm);
  a->bind(&data);
  a->dc64(reinterpret_cast<int64_t>(i.taken));
  a->bind(&skip);
  emit(jmp{i.target});
}

void Vgen::emit(const jmp& i) {
  if (next == i.target) return;
  jmps.push_back({a->frontier(), i.target});
  vixl::Label data;
  a->Ldr(rAsm, &data);
  a->Br(rAsm);
  a->bind(&data);
  a->dc64(reinterpret_cast<int64_t>(a->frontier()));
}

void Vgen::emit(const jmpi& i) {
  vixl::Label data;
  a->Ldr(rAsm, &data);
  a->Br(rAsm);
  a->bind(&data);
  a->dc64(reinterpret_cast<int64_t>(i.target));
}

void Vgen::emit(const jmpm& i) {
  a->Ldr(rAsm, M(i.target));
  a->Br(rAsm);
}

void Vgen::emit(const lea& i) {
  auto adr = M(i.s);
  auto offset = reinterpret_cast<int64_t>(adr.offset());
  a->Add(X(i.d), adr.base(), offset /* Don't set flags */);
}

void Vgen::emit(const loadqp& i) {
  a->Mov(X(i.d), i.s.r.disp);
  a->Ldr(X(i.d), X(i.d)[0]);
}

#define Y(vasm_opc, arm_opc, src_dst, m)                   \
void Vgen::emit(const vasm_opc& i) {                       \
  assertx(i.m.base.isValid());                             \
  a->Mov(rAsm, X(i.m.base));                               \
  if (i.m.index.isValid()) {                               \
    auto shift = (i.m.scale == 2) ? 1 :                    \
                 (i.m.scale == 4) ? 2 :                    \
                 (i.m.scale == 3) ? 3 : 0;                 \
    a->Add(rAsm, rAsm, Operand(X(i.m.index), LSL, shift)); \
  }                                                        \
  if (i.m.disp != 0) {                                     \
    a->Add(rAsm, rAsm, i.m.disp);                          \
  }                                                        \
  a->arm_opc(V(i.src_dst), MemOperand(rAsm));              \
}

Y(loadups, ld1, d, s)
Y(storeups, st1, s, m)

#undef Y

/*
 * Flags
 *   SF, ZF, PF should be updated according to result
 *   CF, OF should be cleared
 *   AF is undefined
 *
 * In the following implementation,
 *   N, Z are updated according to result
 *   C, V are cleared
 *   PF, AF are not available
 */
#define Y(vasm_opc, arm_opc, gpr_w, s0, zr)         \
void Vgen::emit(const vasm_opc& i) {                \
  a->arm_opc(gpr_w(i.d), gpr_w(i.s1), s0);          \
  a->Bic(vixl::zr, gpr_w(i.d), vixl::zr, SetFlags); \
}

Y(orli, Orr, W, i.s0.l(), wzr);
Y(orqi, Orr, X, i.s0.l(), xzr);
Y(orq, Orr, X, X(i.s0), xzr);
Y(xorb, Eor, W, W(i.s0), wzr);
Y(xorbi, Eor, W, i.s0.l(), wzr);
Y(xorl, Eor, W, W(i.s0), wzr);
Y(xorq, Eor, X, X(i.s0), xzr);
Y(xorqi, Eor, X, i.s0.q(), xzr);

#undef Y

void Vgen::emit(const popm& i) {
  a->Ldr(rAsm, MemOperand(sp, 8, PostIndex));
  a->Str(rAsm, M(i.d));
}

void Vgen::emit(const psllq& i) {
  // TODO: Add simd shift support in vixl
  a->Fmov(rAsm, D(i.s1));
  a->Lsl(rAsm, rAsm, i.s0.l());
  a->Fmov(D(i.d), rAsm);
}

void Vgen::emit(const psrlq& i) {
  // TODO: Needs simd shift support in vixl
  a->Fmov(rAsm, D(i.s1));
  a->Lsr(rAsm, rAsm, i.s0.l());
  a->Fmov(D(i.d), rAsm);
}

void Vgen::emit(const pushm& i) {
  a->Ldr(rAsm, M(i.s));
  a->Str(rAsm, MemOperand(sp, -8, PreIndex));
}

void Vgen::emit(const roundsd& i) {
  switch(i.dir) {
    case RoundDirection::nearest: {
      a->frintn(D(i.d), D(i.s));
      break;
    }

    case RoundDirection::floor: {
      a->frintm(D(i.d), D(i.s));
      break;
    }

    case RoundDirection:: ceil: {
      a->frintp(D(i.d), D(i.s));
      break;
    }

    default: {
      assertx(i.dir == RoundDirection::truncate);
      a->frintz(D(i.d), D(i.s));
    }
  }
}

/*
 * Flags
 *   SF, ZF, PF are updated according to result
 *   CF is the last bit shifted out of the operand
 *   OF is defined only if 'count' is 1
 *     For left shifts, OF is set to 0 if the MSB of result is same as CF
 *     (i.e., the top 2 bits of the operand are same). OF is set to 1 otherwise.
 *     For SAR, OF is set to 0. For SHR, OF is set to MSB of original operand
 *   AF is undefined
 *
 * In the following implementation,
 *   N, Z are updated according to result
 *   C, V are undefined (FIXME)
 *   PF, AF are not available
 */
#define Y(vasm_opc, arm_opc, gpr_w, s0, zr)         \
void Vgen::emit(const vasm_opc& i) {                \
  a->arm_opc(gpr_w(i.d), gpr_w(i.s1), s0);          \
  a->Add(vixl::zr, vixl::zr, gpr_w(i.d), SetFlags); \
}

Y(sar, Asr, X, X(i.s0), xzr);
Y(sarqi, Asr, X, i.s0.l(), xzr);
Y(shl, Lsl, X, X(i.s0), xzr);
Y(shlli, Lsl, W, i.s0.l(), wzr);
Y(shlqi, Lsl, X, i.s0.l(), xzr);
Y(shrli, Lsr, W, i.s0.l(), wzr);
Y(shrqi, Lsr, X, i.s0.l(), xzr);

#undef Y

void Vgen::emit(const srem& i) {
  a->Sdiv(rAsm, X(i.s0), X(i.s1));
  a->Msub(X(i.d), rAsm, X(i.s1), X(i.s0));
}

void Vgen::emit(const storebi& i) {
  a->Mov(rAsm.W(), i.s.l());
  a->Strb(rAsm.W(), M(i.m));
}

void Vgen::emit(const storeli& i) {
  a->Mov(rAsm.W(), i.s.l());
  a->Str(rAsm.W(), M(i.m));
}

void Vgen::emit(const storeqi& i) {
  a->Mov(rAsm, i.s.q());
  a->Str(rAsm, M(i.m));
}

void Vgen::emit(const storesd& i) {
  a->Fmov(rAsm, D(i.s));
  a->Str(rAsm, M(i.m));
}

void Vgen::emit(const storewi& i) {
  a->Mov(rAsm.W(), i.s.l());
  a->Strh(rAsm.W(), M(i.m));
}

void Vgen::emit(const unpcklpd& i) {
  a->fmov(D(i.d), D(i.s0));
  a->fmov(rAsm, D(i.s1));
  a->fmov(D(i.d), 1, rAsm);
}

void Vgen::emit(const pushp& i) {
  a->Stp(X(i.s0), X(i.s1), MemOperand(sp, -16, PreIndex));
}

///////////////////////////////////////////////////////////////////////////////

template<typename Lower>
void lower_impl(Vunit& unit, Vlabel b, size_t i, Lower lower) {
  vmodify(unit, b, i, [&] (Vout& v) { lower(v); return 1; });
}

template<typename Inst>
void lower(Vunit& unit, Inst& inst, Vlabel b, size_t i) {}

///////////////////////////////////////////////////////////////////////////////

/*
 * TODO: Using load size (ldr[bh]?), apply scaled address
 */
void lowerVptr(Vptr& p, Vout& v) {
  enum {
    BASE = 1,
    INDEX = 2,
    DISP = 4
  };

  // If p.seg is not Vreg::DS, it is a TLS address and baseless
  if (p.seg != Vptr::DS) {
    assertx(!p.base.isValid());
    auto b = v.makeReg();
    v << mrs{TPIDR_EL0, b};
    p.base = b;
  }

  uint8_t mode = (((p.base.isValid()  & 0x1) << 0) |
                  ((p.index.isValid() & 0x1) << 1) |
                  (((p.disp != 0)     & 0x1) << 2));
  switch(mode) {
    case BASE:
    case INDEX:
    case BASE | INDEX: {
      // ldr/str allow [base] and [base, index], nothing to lower
      break;
    }

    case BASE | DISP: {
      // ldr/str allow [base, #imm], where #imm is [-256 .. 255]
      if (p.disp >= -256 && p.disp <= 255)
        break;
  
      // #imm is out of range, convert to [base, index]
      auto index = v.makeReg();
      v << ldimml{Immed(p.disp), index};
      p.index = index;
      p.scale = 1;
      p.disp = 0;
      break;
    }

    case DISP: {
      // Not supported, convert to [base]
      auto base = v.makeReg();
      v << ldimml{Immed(p.disp), base};
      p.base = base;
      p.index = Vreg{};
      p.scale = 1;
      p.disp = 0;
      break;
    }

    case INDEX | DISP: {
      // Not supported, convert to [base, #imm] or [base, index]
      p.base = p.index;
      if (p.disp >= -256 && p.disp <= 255) {
        p.index = Vreg{};
        p.scale = 1;
      } else {
        auto index = v.makeReg();
        v << ldimml{Immed(p.disp), index};
        p.index = index;
        p.scale = 1;
        p.disp = 0;
      }
      break;
    }

    case BASE | INDEX | DISP: {
      // Not supported, convert to [base, index]
      auto index = v.makeReg();
      if (p.scale > 1) {
        v << shlqinf{p.scale, p.index, index};
        v << addqinf{p.disp, index, index};
      } else {
        v << addqinf{p.disp, p.index, index};
      }
      p.index = index;
      p.scale = 1;
      p.disp = 0;
      break;
    }
  }
}

#define Y(vasm_opc, m)                                  \
void lower(Vunit& u, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(u, b, z, [&] (Vout& v) {                   \
    lowerVptr(i.m, v);                                  \
    v << i;                                             \
  });                                                   \
}

Y(callm, target)
Y(cloadq, t)
Y(incqmlock, m)
Y(jmpm, target)
Y(lea, s)
Y(loadb, s)
Y(loadl, s)
Y(load, s)
Y(loadsd, s)
Y(loadtqb, s)
Y(loadups, s)
Y(loadw, s)
Y(loadzbl, s)
Y(loadzbq, s)
Y(loadzlq, s)
Y(popm, d)
Y(pushm, s)
Y(storebi, m)
Y(storeb, m)
Y(store, d)
Y(storeli, m)
Y(storel, m)
Y(storeqi, m)
Y(storesd, m)
Y(storeups, m)
Y(storewi, m)
Y(storew, m)

#undef Y

#define Y(vasm_opc, lower_opc, load_op, store_op, s0, m) \
void lower(Vunit& u, vasm_opc& i, Vlabel b, size_t z) {  \
  lower_impl(u, b, z, [&] (Vout& v) {                    \
    lowerVptr(i.m, v);                                   \
    auto r = v.makeReg();                                \
    v << load_op{i.m, r};                                \
    v << lower_opc{i.s0, r, r, i.sf};                    \
    v << store_op{r, i.m};                               \
  });                                                    \
}

Y(addlim, addli, loadl, storel, s0, m)
Y(addlm, addl, loadl, storel, s0, m)
Y(addqim, addqi, load, store, s0, m)
Y(andbim, andbi, loadb, storeb, s, m)
Y(orbim, orli, loadb, storeb, s0, m)
Y(orqim, orqi, load, store, s0, m)
Y(orwim, orli, loadw, storew, s0, m)

#undef Y

#define Y(vasm_opc, lower_opc, load_op, s0, m)          \
void lower(Vunit& u, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(u, b, z, [&] (Vout& v) {                   \
    lowerVptr(i.m, v);                                  \
    auto r = v.makeReg();                               \
    v << load_op{i.m, r};                               \
    v << lower_opc{i.s0, r, i.sf};                      \
  });                                                   \
}

Y(cmplim, cmpli, loadl, s0, s1)
Y(cmplm, cmpl, loadl, s0, s1)
Y(cmpqim, cmpqi, load, s0, s1)
Y(cmpqm, cmpq, load, s0, s1)
Y(cmpwim, cmpli, loadw, s0, s1)
Y(testbim, testbi, loadb, s0, s1)
Y(testlim, testli, loadl, s0, s1)
Y(testqim, testqi, load, s0, s1)
Y(testqm, testq, load, s0, s1)
Y(testwim, testli, loadw, s0, s1)

#undef Y

#define Y(vasm_opc)                                     \
void lower(Vunit& u, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(u, b, z, [&] (Vout& v) {                   \
    auto r = v.makeReg();                               \
    v << mrs{NZCV, r};                                  \
    v << i;                                             \
    v << msr{r, NZCV};                                  \
  });                                                   \
}

Y(cmpsd)

#undef Y

void lower(Vunit& u, cvtsi2sdm& i, Vlabel b, size_t z) {
  lower_impl(u, b, z, [&] (Vout& v) {
    lowerVptr(i.s, v);
    auto r = v.makeReg();
    v << load{i.s, r};
    v << cvtsi2sd{r, i.d};
  });
}

#define Y(vasm_opc, lower_opc, load_op, store_op, m)    \
void lower(Vunit& u, vasm_opc& i, Vlabel b, size_t z) { \
  lower_impl(u, b, z, [&] (Vout& v) {                   \
    lowerVptr(i.m, v);                                  \
    auto r = v.makeReg();                               \
    v << load_op{i.m, r};                               \
    v << lower_opc{r, r, i.sf};                         \
    v << store_op{r, i.m};                              \
  });                                                   \
}

Y(declm, decl, loadl, storel, m)
Y(decqm, decq, load, store, m)
Y(inclm, incl, loadl, storel, m)
Y(incqm, incq, load, store, m)
Y(incwm, incw, loadw, storew, m)

#undef Y

void lower(Vunit& u, loadstubret& i, Vlabel b, size_t z) {
  lower_impl(u, b, z, [&] (Vout& v) {
    v << load{rsp()[8], i.d};
  });
}

void lower(Vunit& u, stubtophp& i, Vlabel b, size_t z) {
  lower_impl(u, b, z, [&] (Vout& v) {
    v << lea{rsp()[16], rsp()};
  });
}

void lower(Vunit& u, calltc& i, Vlabel b, size_t z) {
  lower_impl(u, b, z, [&] (Vout& v) {
    // Push FP, LR for callToExit(..)
    auto r0 = v.makeReg();
    auto r1 = v.makeReg();
    v << load{i.fp[AROFF(m_savedRip)], r0};
    v << ldimmq{i.exittc, r1};
    v << pushp{r0, r1};

    // Emit 'calltc'
    v << i;

    // Set the return address to exittc and jump to target
    v << copy{r1, rlink()};
    v << jmpr{i.target, i.args};
  });
}

void lower(Vunit& u, resumetc& i, Vlabel b, size_t z) {
  lower_impl(u, b, z, [&] (Vout& v) {
    v << callr{i.target, i.args};
    v << jmpi{i.exittc};
  });
}

void lower(Vunit& u, cmpb& i, Vlabel b, size_t z) {
  lower_impl(u, b, z, [&] (Vout& v) {
    auto s0 = v.makeReg();
    auto s1 = v.makeReg();
    v << movzbl{i.s0, s0};
    v << movzbl{i.s1, s1};
    v << cmpl{s0, s1, i.sf};
  });
}

void lower(Vunit& u, cmpbi& i, Vlabel b, size_t z) {
  lower_impl(u, b, z, [&] (Vout& v) {
    auto s1 = v.makeReg();
    v << movzbl{i.s1, s1};
    v << cmpli{i.s0, s1, i.sf};
  });
}

void lower(Vunit& u, cmpbim& i, Vlabel b, size_t z) {
  lower_impl(u, b, z, [&] (Vout& v) {
    lowerVptr(i.s1, v);
    auto r = v.makeReg();
    v << loadzbl{i.s1, r};
    v << cmpli{i.s0, r, i.sf};
  });
}

///////////////////////////////////////////////////////////////////////////////

void lower_vcallarray(Vunit& unit, Vlabel b) {
  auto& code = unit.blocks[b].code;
  // vcallarray can only appear at the end of a block.
  auto const inst = code.back().get<vcallarray>();
  auto const origin = code.back().origin;

  auto argRegs = inst.args;
  auto const& srcs = unit.tuples[inst.extraArgs];
  jit::vector<Vreg> dsts;
  for (int i = 0; i < srcs.size(); ++i) {
    dsts.emplace_back(rarg(i));
    argRegs |= rarg(i);
  }

  code.back() = copyargs{unit.makeTuple(srcs), unit.makeTuple(std::move(dsts))};
  code.emplace_back(callarray{inst.target, argRegs});
  code.back().origin = origin;
  code.emplace_back(unwind{{inst.targets[0], inst.targets[1]}});
  code.back().origin = origin;
}

///////////////////////////////////////////////////////////////////////////////

void lowerForARM(Vunit& unit) {
  Timer timer(Timer::vasm_lower);

  // This pass relies on having no critical edges in the unit.
  splitCriticalEdges(unit);

  // Scratch block can change blocks allocation, hence cannot use regular
  // iterators.
  auto& blocks = unit.blocks;

  PostorderWalker{unit}.dfs([&] (Vlabel ib) {
    assertx(!blocks[ib].code.empty());

    auto& back = blocks[ib].code.back();
    if (back.op == Vinstr::vcallarray) {
      lower_vcallarray(unit, Vlabel{ib});
    }

    for (size_t ii = 0; ii < blocks[ib].code.size(); ++ii) {
      vlower(unit, ib, ii);

      auto& inst = blocks[ib].code[ii];
      switch (inst.op) {
#define O(name, ...)                          \
        case Vinstr::name:                    \
          lower(unit, inst.name##_, ib, ii);  \
          break;

        VASM_OPCODES
#undef O
      }
    }
  });

  printUnit(kVasmLowerLevel, "after lower for ARM", unit);
}

///////////////////////////////////////////////////////////////////////////////
}

void optimizeARM(Vunit& unit, const Abi& abi, bool regalloc) {
  Timer timer(Timer::vasm_optimize);

  removeTrivialNops(unit);
  optimizePhis(unit);
  fuseBranches(unit);
  optimizeJmps(unit);
  optimizeExits(unit);

  assertx(checkWidths(unit));

  lowerForARM(unit);
  simplify(unit);

  if (!unit.constToReg.empty()) {
    foldImms<arm::ImmFolder>(unit);
  }

  optimizeCopies(unit, abi);

  if (unit.needsRegAlloc()) {
    removeDeadCode(unit);
    if (regalloc) allocateRegisters(unit, abi);
  }
  if (unit.blocks.size() > 1) {
    optimizeJmps(unit);
  }
}

void emitARM(const Vunit& unit, Vtext& text, CGMeta& fixups,
             AsmInfo* asmInfo) {
  vasm_emit<Vgen>(unit, text, fixups, asmInfo);
}

///////////////////////////////////////////////////////////////////////////////
}}
