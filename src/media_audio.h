// media_audio.h - WASAPI audible-session sensor for media detection. Part of OLED Aegis.

#ifndef MEDIA_AUDIO_H
#define MEDIA_AUDIO_H

// Collect exe names of ACTIVE, AUDIBLE sessions (peak-meter gated; paused/silent ignored). Returns count into names[]; 0 on failure (block-all).
int CollectActiveAudioProcessNames(char names[][MAX_PATH], int maxNames);

#endif // MEDIA_AUDIO_H
