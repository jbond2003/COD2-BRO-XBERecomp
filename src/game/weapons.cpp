// Weapons, where the PC version differs from the console's.
//
// A weapon whose definition says noPartialReload (+0x704) and that takes a whole clip (reloadAmmoAdd, +0x70C, zero or
// the clip's size: the M1 Garand) reloads only when its clip is empty: the check at 0x2BBDC0 (may the weapon in hand
// reload: ammunition in reserve, the clip not full, then this rule) ends at 0x2BBE2B with the rounds left in esi.
// Here it reloads with rounds left as well, as every other weapon does; a weapon loaded a few rounds at a time
// keeps its rule.

#include "cpu/x86_ops.h"

// the rule's last test: the rounds in the clip, none for the reload to go ahead
void ReloadWithRoundsLeft(X86Context& ctx, uint8_t*) { ctx.esi = 0; }
