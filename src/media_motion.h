// media_motion.h - per-monitor content motion probe: moving content counts as media without a window mapping. Part of OLED Aegis.

#ifndef MEDIA_MOTION_H
#define MEDIA_MOTION_H

#define MOTION_GRACE_MS 15000  // Keep the flag while content briefly holds still (quiet scenes)

// Sample every monitor; skipped ones get age = 0xFFFFFFFF (diff/fails stay 0): fullscreen non-media foreground (capture hitch) or saver-covered (frozen).
void RunMotionProbe(int motionDiff[MAX_MONITOR_COUNT],
                    int motionFails[MAX_MONITOR_COUNT],
                    DWORD motionAge[MAX_MONITOR_COUNT]);

// Returns 1 when the foreground window covers >=95% of the monitor (fullscreen game/video).
int IsForegroundFullscreenOnMonitor(int monitorIndex);

#endif // MEDIA_MOTION_H
