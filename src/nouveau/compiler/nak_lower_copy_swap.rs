// Copyright © 2022 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::nak_ir::*;

struct LowerCopySwap {}

impl LowerCopySwap {
    fn new() -> Self {
        Self {}
    }

    fn lower_copy(&mut self, b: &mut impl Builder, copy: OpCopy) {
        let dst_reg = copy.dst.as_reg().unwrap();
        assert!(dst_reg.comps() == 1);
        assert!(copy.src.src_mod.is_none());

        match dst_reg.file() {
            RegFile::GPR => match copy.src.src_ref {
                SrcRef::Zero | SrcRef::Imm32(_) | SrcRef::CBuf(_) => {
                    b.push_op(OpMov {
                        dst: copy.dst,
                        src: copy.src,
                        quad_lanes: 0xf,
                    });
                }
                SrcRef::True | SrcRef::False => {
                    panic!("Cannot copy to GPR");
                }
                SrcRef::Reg(src_reg) => match src_reg.file() {
                    RegFile::GPR => {
                        b.push_op(OpMov {
                            dst: copy.dst,
                            src: copy.src,
                            quad_lanes: 0xf,
                        });
                    }
                    _ => panic!("Cannot copy to GPR"),
                },
                SrcRef::SSA(_) => panic!("Should be run after RA"),
            },
            RegFile::Pred => match copy.src.src_ref {
                SrcRef::Zero | SrcRef::Imm32(_) | SrcRef::CBuf(_) => {
                    panic!("Cannot copy to Pred");
                }
                SrcRef::True => {
                    b.lop2_to(
                        copy.dst,
                        LogicOp::new_const(true),
                        Src::new_imm_bool(true),
                        Src::new_imm_bool(true),
                    );
                }
                SrcRef::False => {
                    b.lop2_to(
                        copy.dst,
                        LogicOp::new_const(false),
                        Src::new_imm_bool(true),
                        Src::new_imm_bool(true),
                    );
                }
                SrcRef::Reg(src_reg) => match src_reg.file() {
                    RegFile::Pred => {
                        b.lop2_to(
                            copy.dst,
                            LogicOp::new_lut(&|x, _, _| x),
                            copy.src,
                            Src::new_imm_bool(true),
                        );
                    }
                    _ => panic!("Cannot copy to Pred"),
                },
                SrcRef::SSA(_) => panic!("Should be run after RA"),
            },
            _ => panic!("Unhandled register file"),
        }
    }

    fn lower_swap(&mut self, b: &mut impl Builder, swap: OpSwap) {
        let x = *swap.dsts[0].as_reg().unwrap();
        let y = *swap.dsts[1].as_reg().unwrap();

        assert!(x.file() == y.file());
        assert!(x.comps() == 1 && y.comps() == 1);
        assert!(swap.srcs[0].src_mod.is_none());
        assert!(*swap.srcs[0].src_ref.as_reg().unwrap() == y);
        assert!(swap.srcs[1].src_mod.is_none());
        assert!(*swap.srcs[1].src_ref.as_reg().unwrap() == x);

        if x == y {
            /* Nothing to do */
        } else if x.is_predicate() {
            b.push_op(OpPLop3 {
                dsts: [x.into(), y.into()],
                srcs: [x.into(), y.into(), Src::new_imm_bool(true)],
                ops: [
                    LogicOp::new_lut(&|_, y, _| y),
                    LogicOp::new_lut(&|x, _, _| x),
                ],
            })
        } else {
            let xor = LogicOp::new_lut(&|x, y, _| x ^ y);
            b.lop2_to(x.into(), xor, x.into(), y.into());
            b.lop2_to(y.into(), xor, x.into(), y.into());
            b.lop2_to(x.into(), xor, x.into(), y.into());
        }
    }

    fn run(&mut self, s: &mut Shader) {
        s.map_instrs(|instr: Box<Instr>, _| -> MappedInstrs {
            match instr.op {
                Op::Copy(copy) => {
                    debug_assert!(instr.pred.is_true());
                    let mut b = InstrBuilder::new();
                    self.lower_copy(&mut b, copy);
                    b.as_mapped_instrs()
                }
                Op::Swap(swap) => {
                    debug_assert!(instr.pred.is_true());
                    let mut b = InstrBuilder::new();
                    self.lower_swap(&mut b, swap);
                    b.as_mapped_instrs()
                }
                _ => MappedInstrs::One(instr),
            }
        });
    }
}

impl Shader {
    pub fn lower_copy_swap(&mut self) {
        let mut pass = LowerCopySwap::new();
        pass.run(self);
    }
}