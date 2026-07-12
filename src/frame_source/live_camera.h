// live_camera.h — Role A
#ifndef LIVE_CAMERA_H
#define LIVE_CAMERA_H

#include "common_types.h"

typedef struct LiveCamera LiveCamera;

/* Opens the camera through the QNX Sensor Framework (camera/camera_api.h) and
 * starts a viewfinder whose callback converts each frame to 3-channel BGR.
 * device_index 0 -> CAMERA_UNIT_1, 1 -> CAMERA_UNIT_2.
 * Returns NULL if the camera can't be opened or the viewfinder won't start.
 * On non-QNX builds this always returns NULL (use the fallback source). */
LiveCamera *live_camera_open(int device_index);

/* Blocks until the next converted frame arrives (5 s allowance for the first
 * frame while the sensor starts, 2 s after that). Returns NULL on timeout.
 * The returned Frame is owned by the LiveCamera and is valid only until the
 * next live_camera_read() or live_camera_close(). */
Frame *live_camera_read(LiveCamera *cam);

void live_camera_close(LiveCamera *cam);

#endif /* LIVE_CAMERA_H */
