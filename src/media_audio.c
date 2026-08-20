// media_audio.c - WASAPI audible-session scan (default render endpoint). Part of OLED Aegis. GUIDs live in media.c (INITGUID); interfaces only here.

#include "oled_aegis.h"
#include "media_audio.h"

// Process name from PID via QueryFullProcessImageNameW (needs only PROCESS_QUERY_LIMITED_INFORMATION; Chromium sandbox denies VM_READ).
static int GetProcessNameFromPid(DWORD pid, char* buffer, int bufferSize)
{
    if (!buffer || bufferSize <= 0 || pid == 0) return 0;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return 0;

    WCHAR wpath[MAX_PATH] = {0};
    DWORD wpathLen = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProcess, 0, wpath, &wpathLen);
    CloseHandle(hProcess);

    if (!ok || wpathLen == 0) return 0;

    // Extract filename from full path (e.g. "C:\...\brave.exe" -> "brave.exe")
    WCHAR* wexe = wpath;
    for (WCHAR* p = wpath; *p; p++)
    {
        if (*p == L'\\') wexe = p + 1;
    }

    // Convert to narrow char (exe names are ASCII)
    int i = 0;
    for (; wexe[i] && i < bufferSize - 1; i++)
    {
        buffer[i] = (char)wexe[i];
    }
    buffer[i] = '\0';

    return buffer[0] != '\0' ? 1 : 0;
}

int CollectActiveAudioProcessNames(char names[][MAX_PATH], DWORD pids[], int maxNames)
{
    if (!names || !pids || maxNames <= 0) return 0;

    IMMDeviceEnumerator* pEnum = NULL;
    IMMDevice* pDevice = NULL;
    IAudioSessionManager2* pSessionManager = NULL;
    IAudioSessionEnumerator* pSessionEnum = NULL;
    int count = 0;

    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator, (void**)&pEnum);
    if (FAILED(hr) || !pEnum)
    {
        LogMessage("Audio: CoCreateInstance failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    hr = pEnum->lpVtbl->GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pDevice);
    if (FAILED(hr) || !pDevice)
    {
        LogMessage("Audio: GetDefaultAudioEndpoint(eRender,eConsole) failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    hr = pDevice->lpVtbl->Activate(pDevice, &IID_IAudioSessionManager2,
                                   CLSCTX_ALL, NULL, (void**)&pSessionManager);
    if (FAILED(hr) || !pSessionManager)
    {
        LogMessage("Audio: Activate(IAudioSessionManager2) failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    hr = pSessionManager->lpVtbl->GetSessionEnumerator(pSessionManager, &pSessionEnum);
    if (FAILED(hr) || !pSessionEnum)
    {
        LogMessage("Audio: GetSessionEnumerator failed hr=0x%08X", (unsigned)hr);
        goto done;
    }

    int sessionCount = 0;
    pSessionEnum->lpVtbl->GetCount(pSessionEnum, &sessionCount);

    for (int i = 0; i < sessionCount && count < maxNames; i++)
    {
        IAudioSessionControl* pControl = NULL;
        if (FAILED(pSessionEnum->lpVtbl->GetSession(pSessionEnum, i, &pControl)) || !pControl)
        {
            continue;
        }

        AudioSessionState state = AudioSessionStateInactive;
        pControl->lpVtbl->GetState(pControl, &state);

        if (state == AudioSessionStateActive)
        {
            // Sessions stay "active" while paused, so gate on the peak meter to filter silent sessions (paused video must not block the saver).
            IAudioMeterInformation* pMeter = NULL;
            if (SUCCEEDED(pControl->lpVtbl->QueryInterface(pControl, &IID_IAudioMeterInformation, (void**)&pMeter)) && pMeter)
            {
                float peak = 0.0f;
                if (SUCCEEDED(pMeter->lpVtbl->GetPeakValue(pMeter, &peak)) &&
                    peak > AUDIO_ACTIVE_PEAK_THRESHOLD)
                {
                    IAudioSessionControl2* pControl2 = NULL;
                    if (SUCCEEDED(pControl->lpVtbl->QueryInterface(pControl, &IID_IAudioSessionControl2,
                                                                   (void**)&pControl2)) && pControl2)
                    {
                        DWORD pid = 0;
                        if (SUCCEEDED(pControl2->lpVtbl->GetProcessId(pControl2, &pid)) && pid != 0)
                        {
                            char procName[MAX_PATH] = {0};
                            if (GetProcessNameFromPid(pid, procName, sizeof(procName)))
                            {
                                int found = 0;
                                for (int j = 0; j < count; j++)
                                {
                                    if (_stricmp(names[j], procName) == 0)
                                    {
                                        found = 1;
                                        break;
                                    }
                                }
                                if (!found)
                                {
                                    strncpy(names[count], procName, MAX_PATH - 1);
                                    names[count][MAX_PATH - 1] = '\0';
                                    // First audible session for this exe; the fullscreen rule matches the foreground window PID against these.
                                    pids[count] = pid;
                                    count++;
                                }
                            }
                        }
                        pControl2->lpVtbl->Release(pControl2);
                    }
                }
                pMeter->lpVtbl->Release(pMeter);
            }
        }
        pControl->lpVtbl->Release(pControl);
    }

done:
    if (pSessionEnum) pSessionEnum->lpVtbl->Release(pSessionEnum);
    if (pSessionManager) pSessionManager->lpVtbl->Release(pSessionManager);
    if (pDevice) pDevice->lpVtbl->Release(pDevice);
    if (pEnum) pEnum->lpVtbl->Release(pEnum);
    return count;
}
