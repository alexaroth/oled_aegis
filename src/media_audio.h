// media_audio.h - WASAPI audible-session sensor for media detection. Part of OLED Aegis.

#ifndef MEDIA_AUDIO_H
#define MEDIA_AUDIO_H

// Collect exe names + PIDs of ACTIVE, AUDIBLE sessions (peak-meter gated; paused/silent ignored). Returns count; 0 if none audible.
// pids[k] is the PID of the first audible session carrying names[k] (the same exe can hold several sessions).
int CollectActiveAudioProcessNames(char names[][MAX_PATH], DWORD pids[], int maxNames);

#endif // MEDIA_AUDIO_H
