// The lance charge's stamina drain by the frame time, not per frame (above 30 FPS a charge drained stamina FPS / 30
// times as fast and ended that much sooner).
//
// Replaces: FUN_028854D8 lanceCharge? and FUN_02886090 lanceCharge2? (r3 = the uPlayer), the lance's charge; phase in
// pad block +7. FUN_028854D8: 0 sets it up and takes a one-time 4.0 stamina to start (0x028855E4, 0x02885704,
// 0x02885774: one per way in), 1 = the wind-up (4.0 every frame inside a motion window, 0x028857E4), 2 = the run
// (4.0 every frame, 0x02885908) until stamina runs out. FUN_02886090: 0 sets up (no cost), 1 takes 4.0 every frame
// (0x028861F4). Each takes it through FUN_02866B18 (a stamina change with the skills applied) with mode 1, which the
// stamina core FUN_028669A0 applies as is; the game's other continuous drains pass mode 0, which it scales by the frame
// time x 0.5. So the charge drained 120 stamina a second at the console's 30 FPS and 240 at 60.
// Status: tested - read in the disassembly; seen with two hunters (30 and 60 FPS) charging from full stamina: 119 / s
// and 4.8 s of run at 30, 239 / s and 2.3 s at 60; with this, 119 / s and 4.8 s at both, the run's speed unchanged.
// The original runs; when it was called in a draining phase, the stamina it took is scaled by the hunter's frame time
// (cUnit+0x1C in 1/60 s, x 0.5: 1.0 at 30 FPS), as windpress.cpp scales its push, so 30 FPS stays exactly the original
// and the one-time cost to start is never scaled.
#include "lance_charge.h"

#include <bit>
#include <cmath>

#include "functions.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kStamina = 0x648;  // pad block: stamina now (f32; +0x64C its maximum)

// the stamina taken since `before`, scaled by the hunter's frame time
void scale_drain(uint8_t* base, uint32_t player, uint32_t pad, float before) {
    const float after = ppc::ldf32(base, pad + kStamina);
    if (!(after < before)) return;
    const float t = ppc::ldf32(base, player + 0x1C) * 0.5f;
    if (!std::isfinite(t) || t <= 0 || t > 50 || std::fabs(t - 1.0f) < 1e-4f) return;
    ppc::st32(base, pad + kStamina, std::bit_cast<uint32_t>(before + (after - before) * t));
}

PPC_FUNC(lance_charge) {
    const uint32_t player = ctx.r[3];
    const uint32_t pad = ppc::ld32(base, player + 0xE30);
    const bool draining = pad && (base[pad + 7] == 1 || base[pad + 7] == 2);
    const float before = draining ? ppc::ldf32(base, pad + kStamina) : 0;
    sub_028854D8__orig(ctx, base);
    if (draining) scale_drain(base, player, pad, before);
}

PPC_FUNC(lance_charge2) {
    const uint32_t player = ctx.r[3];
    const uint32_t pad = ppc::ld32(base, player + 0xE30);
    const bool draining = pad && base[pad + 7] == 1;
    const float before = draining ? ppc::ldf32(base, pad + kStamina) : 0;
    sub_02886090__orig(ctx, base);
    if (draining) scale_drain(base, player, pad, before);
}

}  // namespace

void register_lance_charge() {
    ppc::set_override(0x028854D8, &lance_charge);
    ppc::set_override(0x02886090, &lance_charge2);
}

}  // namespace mh3u::game
