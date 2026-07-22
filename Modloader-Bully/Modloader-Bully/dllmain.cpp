#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <algorithm>
#include <map>
#include <vector>
#include <cstring>
#include "MinHook.h"

namespace fs = std::filesystem;

// ============================================================================
// CONFIGURATION & GLOBALS
// ============================================================================
const std::string MODLOADER_DIR = "modloader/";
const std::string MODS_INI = "modloader/.data/mods.ini";
const std::string CUSTOM_DIR_FOLDER = "modloader/.dir/";
const std::string LOG_FILE = "modloader.txt";

const uint32_t SECTOR_SIZE = 2048;
const uint32_t FAKE_OFFSET_START = 0x00080000;

CRITICAL_SECTION g_CriticalSection;
bool g_EnableLogging = true;

struct FileCandidate {
    std::string modPath;
    int priority;
    fs::file_time_type modTime;
    uintmax_t fileSize;
};

struct StandardReplacement { std::string modPath; uint32_t originalSize; };
struct VirtualEntry { std::string name; std::string modPath; uint32_t sizeInSectors; };

std::map<std::string, std::map<uint32_t, StandardReplacement>> g_StandardReplacements;
std::map<std::string, std::map<uint32_t, VirtualEntry>> g_VirtualEntries;
std::map<uint32_t, VirtualEntry*> g_FakeOffsetLookup;
std::map<std::string, std::string> g_DirRedirections;
std::map<HANDLE, std::string> g_IMGHandles;
std::map<HANDLE, uint32_t> g_CurrentOffset;

std::string g_GameRoot, g_GameRootLower, g_ModloaderLower;

// --- Original Function Pointers ---
typedef HANDLE(WINAPI* OrigCreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
OrigCreateFileA_t OriginalCreateFileA = nullptr;
typedef HANDLE(WINAPI* OrigCreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
OrigCreateFileW_t OriginalCreateFileW = nullptr;
typedef BOOL(WINAPI* OrigReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
OrigReadFile_t OriginalReadFile = nullptr;
typedef DWORD(WINAPI* OrigSetFilePointer_t)(HANDLE, LONG, PLONG, DWORD);
OrigSetFilePointer_t OriginalSetFilePointer = nullptr;
typedef BOOL(WINAPI* OrigSetFilePointerEx_t)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
OrigSetFilePointerEx_t OriginalSetFilePointerEx = nullptr;
typedef DWORD(WINAPI* OrigGetFileSize_t)(HANDLE, LPDWORD);
OrigGetFileSize_t OriginalGetFileSize = nullptr;
typedef BOOL(WINAPI* OrigGetFileSizeEx_t)(HANDLE, PLARGE_INTEGER);
OrigGetFileSizeEx_t OriginalGetFileSizeEx = nullptr;
typedef DWORD(WINAPI* OrigGetFileAttributesA_t)(LPCSTR);
OrigGetFileAttributesA_t OriginalGetFileAttributesA = nullptr;
typedef DWORD(WINAPI* OrigGetFileAttributesW_t)(LPCWSTR);
OrigGetFileAttributesW_t OriginalGetFileAttributesW = nullptr;

// ============================================================================
// LOGGING & HELPERS
// ============================================================================
void LoadSettings() {
    std::string iniPath = MODLOADER_DIR + "modloader.ini";
    if (!fs::exists(iniPath)) {
        fs::create_directories(MODLOADER_DIR);
        WritePrivateProfileStringA("Settings", "Debug", "1", iniPath.c_str());
        g_EnableLogging = true;
    }
    else {
        char buffer[16];
        GetPrivateProfileStringA("Settings", "Debug", "1", buffer, sizeof(buffer), iniPath.c_str());
        g_EnableLogging = (std::string(buffer) == "1");
    }
}

void Log(const std::string& message) {
    if (!g_EnableLogging) return;
    std::ofstream logFile(LOG_FILE, std::ios::app);
    if (logFile.is_open()) logFile << message << std::endl;
}

std::string ToLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    std::replace(str.begin(), str.end(), '\\', '/');
    return str;
}

std::string ExtractIMGKey(const std::string& fullPath) {
    std::string lower = ToLower(fullPath);
    size_t lastSlash = lower.find_last_of('/');
    if (lastSlash != std::string::npos) return lower.substr(lastSlash + 1);
    return lower;
}

void InitPaths() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    g_GameRoot = path;
    size_t pos = g_GameRoot.find_last_of("\\/");
    g_GameRoot = g_GameRoot.substr(0, pos + 1);
    g_GameRootLower = ToLower(g_GameRoot);
    g_ModloaderLower = ToLower(MODLOADER_DIR);
}

// ============================================================================
// SCANNING & DIR GENERATION (Simplified & Reliable)
// ============================================================================
void EnsureModInIni(const std::string& modName) {
    char buffer[64];
    DWORD res = GetPrivateProfileStringA(modName.c_str(), "Enabled", NULL, buffer, sizeof(buffer), MODS_INI.c_str());
    if (res == 0) {
        WritePrivateProfileStringA(modName.c_str(), "Enabled", "1", MODS_INI.c_str());
        WritePrivateProfileStringA(modName.c_str(), "Priority", "50", MODS_INI.c_str());
    }
}

std::string FindOriginalDir(const std::string& imgKey) {
    std::string baseName = imgKey;
    size_t lastSlash = baseName.find_last_of("/\\");
    if (lastSlash != std::string::npos) baseName = baseName.substr(lastSlash + 1);
    std::string dirFileName = baseName.substr(0, baseName.length() - 4) + ".dir";

    std::vector<std::string> searchPaths = {
        "Stream/" + dirFileName, "stream/" + dirFileName,
        "Config/" + dirFileName, "config/" + dirFileName,
        "Cuts/" + dirFileName, "cuts/" + dirFileName,
        dirFileName,
        "Dat/" + dirFileName, "dat/" + dirFileName, "DAT/" + dirFileName,
        "Objects/" + dirFileName, "objects/" + dirFileName,
        "Scripts/" + dirFileName, "scripts/" + dirFileName,
        "Act/" + dirFileName, "act/" + dirFileName
    };
    for (const auto& p : searchPaths) if (fs::exists(p)) return p;
    return "";
}

void ScanAndProcessMods() {
    fs::create_directories(MODLOADER_DIR + ".data/");
    fs::create_directories(CUSTOM_DIR_FOLDER);

    std::map<std::string, std::map<std::string, FileCandidate>> winners;

    for (const auto& entry : fs::directory_iterator(MODLOADER_DIR)) {
        if (!entry.is_directory()) continue;
        std::string folderName = entry.path().filename().string();
        std::string lowerName = ToLower(folderName);
        if (lowerName == ".dir" || lowerName == ".data") continue;

        EnsureModInIni(folderName);

        bool enabled = GetPrivateProfileIntA(folderName.c_str(), "Enabled", 1, MODS_INI.c_str()) != 0;
        int priority = GetPrivateProfileIntA(folderName.c_str(), "Priority", 50, MODS_INI.c_str());

        if (!enabled) {
            Log("Skipping disabled mod: " + folderName);
            continue;
        }

        Log("Scanning: " + folderName + " (Pri: " + std::to_string(priority) + ")");

        for (const auto& file : fs::recursive_directory_iterator(entry.path())) {
            if (!file.is_regular_file()) continue;
            std::string lowerPath = ToLower(file.path().string());

            size_t imgPos = lowerPath.find(".img");
            if (imgPos != std::string::npos && imgPos + 4 < lowerPath.size() && (lowerPath[imgPos + 4] == '/' || lowerPath[imgPos + 4] == '\\')) {
                std::string beforeImg = lowerPath.substr(0, imgPos);
                size_t startKey = beforeImg.find_last_of("/\\");
                std::string imgKey = (startKey != std::string::npos ? beforeImg.substr(startKey + 1) : beforeImg) + ".img";
                std::string fileName = lowerPath.substr(imgPos + 5);

                auto& imgMap = winners[imgKey];
                auto it = imgMap.find(fileName);

                bool shouldReplace = false;
                if (it == imgMap.end()) shouldReplace = true;
                else if (priority > it->second.priority) shouldReplace = true;
                else if (priority == it->second.priority && file.last_write_time() < it->second.modTime) shouldReplace = true;

                if (shouldReplace) {
                    imgMap[fileName] = { file.path().string(), priority, file.last_write_time(), fs::file_size(file.path()) };
                }
            }
        }
    }

#pragma pack(push, 1)
    struct DirEntry32 { uint32_t offset; uint32_t size; char name[24]; };
#pragma pack(pop)

    for (const auto& imgPair : winners) {
        const std::string& imgKey = imgPair.first;
        std::string originalDirPath = FindOriginalDir(imgKey);
        if (originalDirPath.empty()) continue;

        std::ifstream origDirFile(originalDirPath, std::ios::binary);
        std::vector<char> origDirData((std::istreambuf_iterator<char>(origDirFile)), std::istreambuf_iterator<char>());
        origDirFile.close();
        if (origDirData.size() % 32 != 0) continue;

        std::vector<DirEntry32> newDirEntries;
        std::map<std::string, DirEntry32*> origMap;

        for (size_t i = 0; i < origDirData.size() / 32; i++) {
            DirEntry32* e = reinterpret_cast<DirEntry32*>(origDirData.data() + i * 32);
            newDirEntries.push_back(*e);
            std::string name(e->name, strnlen(e->name, 24));
            name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());
            origMap[ToLower(name)] = &newDirEntries.back();
        }

        uint32_t nextFakeSector = FAKE_OFFSET_START;
        int stdCount = 0, virtCount = 0;

        for (const auto& filePair : imgPair.second) {
            const std::string& lowerName = filePair.first;
            const FileCandidate& data = filePair.second;
            uint32_t modSizeSectors = static_cast<uint32_t>((data.fileSize + SECTOR_SIZE - 1) / SECTOR_SIZE);
            auto it = origMap.find(lowerName);

            if (it != origMap.end()) {
                DirEntry32* origEntry = it->second;
                if (modSizeSectors <= origEntry->size) {
                    g_StandardReplacements[imgKey][origEntry->offset * SECTOR_SIZE] = { data.modPath, origEntry->size * SECTOR_SIZE };
                    stdCount++;
                }
                else {
                    g_VirtualEntries[imgKey][nextFakeSector] = { lowerName, data.modPath, modSizeSectors };
                    g_FakeOffsetLookup[nextFakeSector] = &g_VirtualEntries[imgKey][nextFakeSector];
                    origEntry->offset = nextFakeSector; origEntry->size = modSizeSectors;
                    nextFakeSector++; virtCount++;
                }
            }
            else {
                g_VirtualEntries[imgKey][nextFakeSector] = { lowerName, data.modPath, modSizeSectors };
                g_FakeOffsetLookup[nextFakeSector] = &g_VirtualEntries[imgKey][nextFakeSector];
                DirEntry32 newEntry = { nextFakeSector, modSizeSectors, {} };
                strncpy(newEntry.name, lowerName.c_str(), 23); newEntry.name[23] = '\0';
                newDirEntries.push_back(newEntry);
                nextFakeSector++; virtCount++;
            }
        }

        Log("[" + imgKey + "] Finalized: " + std::to_string(stdCount) + " standard, " + std::to_string(virtCount) + " virtual.");

        std::string customDirPath = CUSTOM_DIR_FOLDER + originalDirPath;
        fs::create_directories(fs::path(customDirPath).parent_path());
        std::ofstream outDir(customDirPath, std::ios::binary);
        if (outDir.is_open()) {
            outDir.write(reinterpret_cast<const char*>(newDirEntries.data()), newDirEntries.size() * 32);
            outDir.close();
            g_DirRedirections[ToLower(originalDirPath)] = customDirPath;
        }
    }
}

// ============================================================================
// THE HOOKS
// ============================================================================
std::string ResolveAndRedirectPath(const std::string& requestedPath) {
    char absPath[MAX_PATH]; GetFullPathNameA(requestedPath.c_str(), MAX_PATH, absPath, NULL);
    std::string absLower = ToLower(absPath);
    if (absLower.find(g_ModloaderLower) != std::string::npos) return "";
    for (const auto& pair : g_DirRedirections) if (absLower.find(pair.first) != std::string::npos) return pair.second;
    return "";
}

HANDLE WINAPI HookedCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSec, DWORD dwDisp, DWORD dwFlags, HANDLE hTemp) {
    if (!lpFileName) return OriginalCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
    std::string r = ResolveAndRedirectPath(lpFileName);
    HANDLE hFile = OriginalCreateFileA(r.empty() ? lpFileName : r.c_str(), dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
    if (hFile != INVALID_HANDLE_VALUE) {
        std::string key = ExtractIMGKey(r.empty() ? lpFileName : r);
        if (key.length() > 4 && key.substr(key.length() - 4) == ".img") {
            EnterCriticalSection(&g_CriticalSection); g_IMGHandles[hFile] = key; g_CurrentOffset[hFile] = 0; LeaveCriticalSection(&g_CriticalSection);
        }
    }
    return hFile;
}

HANDLE WINAPI HookedCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSec, DWORD dwDisp, DWORD dwFlags, HANDLE hTemp) {
    if (!lpFileName) return OriginalCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
    std::wstring wpath(lpFileName); std::string path(wpath.begin(), wpath.end());
    std::string r = ResolveAndRedirectPath(path);
    HANDLE hFile;
    if (!r.empty()) { std::wstring wr(r.begin(), r.end()); hFile = OriginalCreateFileW(wr.c_str(), dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp); }
    else hFile = OriginalCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);

    if (hFile != INVALID_HANDLE_VALUE) {
        std::string key = ExtractIMGKey(r.empty() ? path : r);
        if (key.length() > 4 && key.substr(key.length() - 4) == ".img") {
            EnterCriticalSection(&g_CriticalSection); g_IMGHandles[hFile] = key; g_CurrentOffset[hFile] = 0; LeaveCriticalSection(&g_CriticalSection);
        }
    }
    return hFile;
}

DWORD WINAPI HookedSetFilePointer(HANDLE hFile, LONG lDist, PLONG pDistHigh, DWORD dwMethod) {
    DWORD res = OriginalSetFilePointer(hFile, lDist, pDistHigh, dwMethod);
    if (res != INVALID_SET_FILE_POINTER) { EnterCriticalSection(&g_CriticalSection); if (g_IMGHandles.count(hFile)) g_CurrentOffset[hFile] = res; LeaveCriticalSection(&g_CriticalSection); }
    return res;
}

BOOL WINAPI HookedSetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDist, PLARGE_INTEGER pNew, DWORD dwMethod) {
    BOOL res = OriginalSetFilePointerEx(hFile, liDist, pNew, dwMethod);
    if (res && pNew) { EnterCriticalSection(&g_CriticalSection); if (g_IMGHandles.count(hFile)) g_CurrentOffset[hFile] = pNew->LowPart; LeaveCriticalSection(&g_CriticalSection); }
    return res;
}

DWORD WINAPI HookedGetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    bool isIMG = false; EnterCriticalSection(&g_CriticalSection); if (g_IMGHandles.count(hFile)) isIMG = true; LeaveCriticalSection(&g_CriticalSection);
    if (isIMG) { if (lpFileSizeHigh) *lpFileSizeHigh = 0; return 0x7FFFFFFF; }
    return OriginalGetFileSize(hFile, lpFileSizeHigh);
}

BOOL WINAPI HookedGetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    bool isIMG = false; EnterCriticalSection(&g_CriticalSection); if (g_IMGHandles.count(hFile)) isIMG = true; LeaveCriticalSection(&g_CriticalSection);
    if (isIMG) { lpFileSize->QuadPart = 0x7FFFFFFF; return TRUE; }
    return OriginalGetFileSizeEx(hFile, lpFileSize);
}

DWORD WINAPI HookedGetFileAttributesA(LPCSTR lpFileName) {
    if (lpFileName) { std::string r = ResolveAndRedirectPath(lpFileName); if (!r.empty()) return OriginalGetFileAttributesA(r.c_str()); }
    return OriginalGetFileAttributesA(lpFileName);
}

DWORD WINAPI HookedGetFileAttributesW(LPCWSTR lpFileName) {
    if (lpFileName) {
        std::wstring wpath(lpFileName); std::string path(wpath.begin(), wpath.end());
        std::string r = ResolveAndRedirectPath(path);
        if (!r.empty()) { std::wstring wr(r.begin(), r.end()); return OriginalGetFileAttributesW(wr.c_str()); }
    }
    return OriginalGetFileAttributesW(lpFileName);
}

BOOL WINAPI HookedReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nBytesToRead, LPDWORD pBytesRead, LPOVERLAPPED lpOver) {
    std::string modPath = "";
    uint32_t currentOffset = 0;
    bool isOverlapped = (lpOver != nullptr);

    EnterCriticalSection(&g_CriticalSection);
    auto it = g_IMGHandles.find(hFile);
    if (it != g_IMGHandles.end()) {
        std::string imgKey = it->second;
        uint32_t offset = isOverlapped ? lpOver->Offset : g_CurrentOffset[hFile];
        currentOffset = offset;
        uint32_t sectorOffset = offset / SECTOR_SIZE;

        auto virtIt = g_FakeOffsetLookup.find(sectorOffset);
        if (virtIt != g_FakeOffsetLookup.end()) modPath = virtIt->second->modPath;
        else {
            auto stdIt = g_StandardReplacements.find(imgKey);
            if (stdIt != g_StandardReplacements.end()) {
                auto repIt = stdIt->second.find(offset);
                if (repIt != stdIt->second.end()) modPath = repIt->second.modPath;
            }
        }
    }
    LeaveCriticalSection(&g_CriticalSection);

    if (!modPath.empty()) {
        std::ifstream modFile(modPath, std::ios::binary);
        if (modFile.is_open()) {
            memset(lpBuffer, 0, nBytesToRead);
            modFile.read(static_cast<char*>(lpBuffer), nBytesToRead);
            DWORD bytesRead = static_cast<DWORD>(modFile.gcount());
            if (pBytesRead) *pBytesRead = bytesRead;
            if (lpOver && lpOver->hEvent) SetEvent(lpOver->hEvent);
            else { EnterCriticalSection(&g_CriticalSection); g_CurrentOffset[hFile] += bytesRead; LeaveCriticalSection(&g_CriticalSection); }
            return TRUE;
        }
    }
    return OriginalReadFile(hFile, lpBuffer, nBytesToRead, pBytesRead, lpOver);
}

// ============================================================================
// INITIALIZATION & ENTRY POINT
// ============================================================================
void InitializeHooks() {
    LoadSettings();
    if (g_EnableLogging) std::ofstream(LOG_FILE, std::ios::trunc).close();
    Log("Bully Modloader initialized.");

    InitPaths();
    ScanAndProcessMods();

    if (MH_Initialize() != MH_OK) { Log("ERROR: MinHook init failed!"); return; }
    MH_CreateHook(&CreateFileA, &HookedCreateFileA, (LPVOID*)&OriginalCreateFileA); MH_EnableHook(&CreateFileA);
    MH_CreateHook(&CreateFileW, &HookedCreateFileW, (LPVOID*)&OriginalCreateFileW); MH_EnableHook(&CreateFileW);
    MH_CreateHook(&GetFileAttributesA, &HookedGetFileAttributesA, (LPVOID*)&OriginalGetFileAttributesA); MH_EnableHook(&GetFileAttributesA);
    MH_CreateHook(&GetFileAttributesW, &HookedGetFileAttributesW, (LPVOID*)&OriginalGetFileAttributesW); MH_EnableHook(&GetFileAttributesW);
    MH_CreateHook(&ReadFile, &HookedReadFile, (LPVOID*)&OriginalReadFile); MH_EnableHook(&ReadFile);
    MH_CreateHook(&SetFilePointer, &HookedSetFilePointer, (LPVOID*)&OriginalSetFilePointer); MH_EnableHook(&SetFilePointer);
    MH_CreateHook(&SetFilePointerEx, &HookedSetFilePointerEx, (LPVOID*)&OriginalSetFilePointerEx); MH_EnableHook(&SetFilePointerEx);
    MH_CreateHook(&GetFileSize, &HookedGetFileSize, (LPVOID*)&OriginalGetFileSize); MH_EnableHook(&GetFileSize);
    MH_CreateHook(&GetFileSizeEx, &HookedGetFileSizeEx, (LPVOID*)&OriginalGetFileSizeEx); MH_EnableHook(&GetFileSizeEx);

    Log("All hooks enabled. Modloader is fully ready.");
}

DWORD WINAPI ModloaderThread(LPVOID) {
    Sleep(1000);
    InitializeCriticalSection(&g_CriticalSection);
    InitializeHooks();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, ModloaderThread, NULL, 0, NULL);
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        MH_Uninitialize();
        DeleteCriticalSection(&g_CriticalSection);
    }
    return TRUE;
}