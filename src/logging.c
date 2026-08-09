// logging.c - Debug log file management (append + size-based rotation).
//
// Part of OLED Aegis. See oled_aegis.h for the shared types/constants.

#include "oled_aegis.h"

static char g_logFilePath[MAX_PATH];
FILE* g_logFile = NULL;

static void RotateLogFileIfNeeded() {
    if (!g_logFile) return;

    // Check current file size
    long pos = ftell(g_logFile);
    if (pos < 0 || pos < MAX_LOG_SIZE_BYTES) return;

    // Close current log file
    fclose(g_logFile);
    g_logFile = NULL;

    // Create path for old log file
    char oldLogPath[MAX_PATH];
    sprintf_s(oldLogPath, MAX_PATH, "%s.old", g_logFilePath);

    // Delete existing .old file and rename current to .old
    DeleteFileA(oldLogPath);
    MoveFileA(g_logFilePath, oldLogPath);

    // Reopen fresh log file
    g_logFile = fopen(g_logFilePath, "a");
    if (g_logFile) {
        time_t now = time(NULL);
        char timeStr[64];
        ctime_s(timeStr, sizeof(timeStr), &now);
        timeStr[24] = '\0';
        fprintf(g_logFile, "\n=== Log rotated at %s (previous log saved as .old) ===\n", timeStr);
        fflush(g_logFile);
    }
}

void LogMessage(const char* format, ...) {
    if (!g_app.config.debugMode) return;

    if (!g_logFile) {
        char appDataPath[MAX_PATH];
        GetAppDataPath(appDataPath, sizeof(appDataPath));
        sprintf_s(g_logFilePath, MAX_PATH, "%s\\oled_aegis_debug.log", appDataPath);

        g_logFile = fopen(g_logFilePath, "a");
        if (g_logFile) {
            time_t now = time(NULL);
            char timeStr[64];
            ctime_s(timeStr, sizeof(timeStr), &now);
            timeStr[24] = '\0';
            fprintf(g_logFile, "\n=== OLED Aegis Started at %s ===\n", timeStr);
            fflush(g_logFile);
        }
    }

    if (g_logFile) {
        // Check if log rotation is needed
        RotateLogFileIfNeeded();

        time_t now = time(NULL);
        char timeStr[64];
        ctime_s(timeStr, sizeof(timeStr), &now);
        timeStr[24] = '\0';

        va_list args;
        va_start(args, format);
        fprintf(g_logFile, "[%s] ", timeStr);
        vfprintf(g_logFile, format, args);
        fprintf(g_logFile, "\n");
        fflush(g_logFile);
        va_end(args);
    }
}
