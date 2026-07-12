#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "../../include/common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * State machine (Role C).
 *
 * Consolidates instability + zone-breach signals into a single SystemState:
 *
 *   NORMAL         --instability---------------> HAZARD_ACTIVE
 *   NORMAL         --instability + zone--------> ALARMING
 *   HAZARD_ACTIVE  --zone--------------------> ALARMING
 *   HAZARD_ACTIVE  --stable (N frames)-------> NORMAL
 *   ALARMING       --no zone-----------------> HAZARD_ACTIVE
 *   ALARMING       --stable (N frames)-------> NORMAL
 *
 * "instability" is any of: motion, reference-occupancy, angle (OR'd by caller).
 * Hysteresis via `stable_threshold` prevents flapping when a detector oscillates.
 * -------------------------------------------------------------------------- */

typedef struct StateMachine StateMachine;

/* stable_threshold : consecutive stable frames required to return to NORMAL. */
StateMachine *state_machine_create(int stable_threshold);
void          state_machine_destroy(StateMachine *sm);

/* Feed the frame's fused signals; returns the new state (same as get_state). */
SystemState   state_machine_update(StateMachine *sm,
                                   bool instability, bool zone_breach);

SystemState   state_machine_get_state(const StateMachine *sm);

/* String label for logging/overlay. */
const char   *state_machine_label(SystemState s);

#ifdef __cplusplus
}
#endif

#endif /* STATE_MACHINE_H */
