/* -----------------------------------------------------------------------------
 * state_machine.c — Role C
 *
 * Fuses instability + zone-breach into the global SystemState with hysteresis
 * on the return-to-normal edge.
 * -------------------------------------------------------------------------- */

#include "state_machine.h"

#include <stdlib.h>

struct StateMachine {
    SystemState current;
    int         stable_count;
    int         stable_threshold;
};

StateMachine *state_machine_create(int stable_threshold)
{
    StateMachine *sm = (StateMachine *)calloc(1, sizeof(StateMachine));
    if (!sm) return NULL;
    sm->current          = STATE_NORMAL;
    sm->stable_count     = 0;
    sm->stable_threshold = (stable_threshold < 1) ? 1 : stable_threshold;
    return sm;
}

void state_machine_destroy(StateMachine *sm)
{
    free(sm);
}

SystemState state_machine_update(StateMachine *sm,
                                 bool instability, bool zone_breach)
{
    if (!sm) return STATE_NORMAL;

    if (instability) {
        sm->stable_count = 0;
        sm->current = zone_breach ? STATE_ALARMING : STATE_HAZARD_ACTIVE;
        return sm->current;
    }

    /* No instability this frame — apply hysteresis before dropping to NORMAL. */
    if (sm->current == STATE_NORMAL) {
        sm->stable_count = 0;
        return sm->current;
    }

    sm->stable_count++;
    if (sm->stable_count >= sm->stable_threshold) {
        sm->current = STATE_NORMAL;
        sm->stable_count = 0;
    }
    /* Otherwise keep the previous state (HAZARD_ACTIVE or ALARMING) until debounce clears. */
    return sm->current;
}

SystemState state_machine_get_state(const StateMachine *sm)
{
    return sm ? sm->current : STATE_NORMAL;
}

const char *state_machine_label(SystemState s)
{
    switch (s) {
        case STATE_NORMAL:        return "NORMAL";
        case STATE_HAZARD_ACTIVE: return "HAZARD_ACTIVE";
        case STATE_ALARMING:      return "ALARMING";
        default:                  return "UNKNOWN";
    }
}
