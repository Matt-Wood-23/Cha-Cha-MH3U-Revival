// The hammer spin counted by loops, not by the frame time (above 30 FPS the golf swing out of the spin could not be
// done: every press ended the spin in the backward swing).
//
// Replaces: FUN_0287F638 hammerSpin? (r3 = the uPlayer), the spin after a level-3 charge is released while moving
// (hammer action 0x18; pad block +8 counts the loops down from 4). At each loop end (motion frame 44, window check
// FUN_0285DE94) it keeps pad +0x90 in +0x94 and adds *(pad +0x98) to +0x90: the hunter's frame time x 0.5
// (uPlayer +0x27A8: 1.0 at the console's 30 FPS, 0.5 at 60). While loops remain and +0x90 >= 2.0 (0x1007DEBC) it
// swaps the spin's move list 0x102AD874 (backward swing, action 0x1B) for 0x102AD8B4 (golf swing, 0x1A) through
// FUN_028B2C4C; the press is taken at the last loop end (motion frames 44-45). At 30 FPS +0x90 reads 1, 2, 3 at the
// loop ends and the golf list is in from the second; at 60 it reads 0.5, 1.0, 1.5 and the golf list never goes in.
// Status: tested - read in the disassembly; seen with two hunters on the same input (charge to level 3, walk,
// release, the attack button 0.1 s before the last loop ends): golf swing at 30 FPS, backward swing at 60; with this,
// the golf swing at both and +0x90 1, 2, 3 at both.
// The original runs with +0x98 pointed at a constant 1.0 (0x10029C08) for the call, as grab_escape.cpp does, so each
// loop counts 1 at every frame rate; nothing else in the call reads that pointer (FUN_0287F638 and the functions it
// calls, three levels down).
#include "hammer_spin.h"

#include "functions.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kOne = 0x10029C08;  // .rodata float 1.0

PPC_FUNC(hammer_spin) {
    const uint32_t pad = ppc::ld32(base, ctx.r[3] + 0xE30);
    if (!pad) {
        sub_0287F638__orig(ctx, base);
        return;
    }
    const uint32_t frame_time = ppc::ld32(base, pad + 0x98);
    ppc::st32(base, pad + 0x98, kOne);
    sub_0287F638__orig(ctx, base);
    ppc::st32(base, pad + 0x98, frame_time);
}

}  // namespace

void register_hammer_spin() { ppc::set_override(0x0287F638, &hammer_spin); }

}  // namespace mh3u::game
