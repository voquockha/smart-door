#ifndef LOCKER_UI_H
#define LOCKER_UI_H

#include <stdint.h>
#include "locker_tick.h"

enum class LockerMode {
    None = 0,
    Deposit,
    Pickup
};

bool locker_ui_init();
void locker_ui_process();
void locker_ui_shutdown();

bool locker_ui_is_scanning();
LockerMode locker_ui_mode();

/* BGR888 camera frame. Safe to call only from the LVGL/main thread. */
void locker_ui_update_camera(const uint8_t *bgr, int width, int height,
                             int stride);
void locker_ui_set_hint(const char *hint);
/*
 * Processes one live FaceNet embedding for the active operation.
 * Deposit averages several frames and saves a temporary identity.
 * Pickup matches only identities currently associated with occupied lockers.
 * Returns true when the operation has completed and scanning has stopped.
 */
bool locker_ui_process_embedding(const float *embedding, int dimension,
                                 float liveness_score);
void locker_ui_face_rejected(const char *reason);

#endif
