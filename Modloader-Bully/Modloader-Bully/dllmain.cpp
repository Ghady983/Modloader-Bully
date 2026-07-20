#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <map>
#include <vector>
#include <iterator>
#include <cstring>
#include "MinHook.h"

namespace fs = std::filesystem;

// ============================================================================
// CONFIGURATION & GLOBALS
// ============================================================================
const std::string MODLOADER_DIR = "modloader/";
const std::string MODLOADER_INI = "modloader/modloader.ini"; // NEW: Config file
const std::string CUSTOM_DIR_FOLDER = "modloader/.dir/";
const std::string DATA_DIR = "modloader/.data/";
const std::string PRIORITY_INI = "modloader/.data/priority.ini";
const std::string LOG_FILE = "modloader.txt";

const uint32_t SECTOR_SIZE = 2048;
const uint32_t FAKE_OFFSET_START = 0x00080000;
const int DEFAULT_PRIORITY = 50;

CRITICAL_SECTION g_CriticalSection;
bool g_EnableLogging = true; // NEW: Logging toggle

// --- Data Structures ---
struct FileCandidate {
    std::string fullPath;
    int priority;
    fs::file_time_type modTime;
    uintmax_t fileSize;
};

struct StandardReplacement { std::string modPath; uint32_t originalSize; };
struct VirtualEntry { std::string name; std::string modPath; uint32_t sizeInSectors; };

// --- Global Maps ---
std::map<std::string, std::map<std::string, FileCandidate>> g_WinningCandidates;
std::map<std::string, FileCandidate> g_LooseFileCandidates;

std::map<std::string, std::map<uint32_t, StandardReplacement>> g_StandardReplacements;
std::map<std::string, std::map<uint32_t, VirtualEntry>> g_VirtualEntries;
std::map<uint32_t, VirtualEntry*> g_FakeOffsetLookup;
std::map<std::string, std::string> g_DirRedirections;
std::map<HANDLE, std::string> g_IMGHandles;
std::map<HANDLE, uint32_t> g_CurrentOffset;

std::string g_GameRoot;
std::string g_GameRootLower;
std::string g_ModloaderLower;

// --- Original Function Pointers ---
typedef HANDLE(WINAPI* OriginalCreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
OriginalCreateFileA_t OriginalCreateFileA = nullptr;
typedef HANDLE(WINAPI* OriginalCreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
OriginalCreateFileW_t OriginalCreateFileW = nullptr;
typedef BOOL(WINAPI* OriginalReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
OriginalReadFile_t OriginalReadFile = nullptr;
typedef DWORD(WINAPI* OriginalSetFilePointer_t)(HANDLE, LONG, PLONG, DWORD);
OriginalSetFilePointer_t OriginalSetFilePointer = nullptr;
typedef BOOL(WINAPI* OriginalSetFilePointerEx_t)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
OriginalSetFilePointerEx_t OriginalSetFilePointerEx = nullptr;
typedef DWORD(WINAPI* OriginalGetFileSize_t)(HANDLE, LPDWORD);
OriginalGetFileSize_t OriginalGetFileSize = nullptr;
typedef BOOL(WINAPI* OriginalGetFileSizeEx_t)(HANDLE, PLARGE_INTEGER);
OriginalGetFileSizeEx_t OriginalGetFileSizeEx = nullptr;
typedef DWORD(WINAPI* OriginalGetFileAttributesA_t)(LPCSTR);
OriginalGetFileAttributesA_t OriginalGetFileAttributesA = nullptr;
typedef DWORD(WINAPI* OriginalGetFileAttributesW_t)(LPCWSTR);
OriginalGetFileAttributesW_t OriginalGetFileAttributesW = nullptr;

// ============================================================================
// LOGGING & SETTINGS
// ============================================================================

// NEW: Load settings from modloader.ini
void LoadSettings() {
    if (!fs::exists(MODLOADER_INI)) {
        // If INI doesn't exist, create it with default settings
        fs::create_directories(MODLOADER_DIR);
        WritePrivateProfileStringA("Settings", "Debug", "0", MODLOADER_INI.c_str());
        g_EnableLogging = true;
    }
    else {
        // Read the Debug value
        char buffer[16];
        GetPrivateProfileStringA("Settings", "Debug", "1", buffer, sizeof(buffer), MODLOADER_INI.c_str());
        g_EnableLogging = (std::string(buffer) == "1");
    }
}

void Log(const std::string& message) {
    // If logging is disabled, exit immediately to save performance
    if (!g_EnableLogging) return;

    EnterCriticalSection(&g_CriticalSection);
    std::ofstream logFile(LOG_FILE, std::ios::app);
    if (logFile.is_open()) logFile << message << std::endl;
    LeaveCriticalSection(&g_CriticalSection);
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
// PRIORITY INI MANAGEMENT
// ============================================================================
int GetModPriority(const std::string& modName) {
    char buffer[64];
    DWORD result = GetPrivateProfileStringA("Priorities", modName.c_str(), NULL, buffer, sizeof(buffer), PRIORITY_INI.c_str());
    if (result == 0) {
        WritePrivateProfileStringA("Priorities", modName.c_str(), std::to_string(DEFAULT_PRIORITY).c_str(), PRIORITY_INI.c_str());
        Log("[INI] Added new mod '" + modName + "' with default score " + std::to_string(DEFAULT_PRIORITY));
        return DEFAULT_PRIORITY;
    }
    return std::stoi(buffer);
}

// ============================================================================
// PHASE 1: THE ELECTION (Recursive Scan)
// ============================================================================
void ScanSingleModFolder(const std::string& modFolderPath, int modPriority) {
    std::string modName = fs::path(modFolderPath).filename().string();

    for (const auto& entry : fs::recursive_directory_iterator(modFolderPath)) {
        if (!entry.is_regular_file()) continue;

        std::string filePath = entry.path().string();
        std::string lowerPath = ToLower(filePath);

        if (lowerPath.find(".img/") != std::string::npos || lowerPath.find(".img\\") != std::string::npos) {
            size_t imgPos = lowerPath.find(".img/");
            if (imgPos == std::string::npos) imgPos = lowerPath.find(".img\\");

            size_t startKey = lowerPath.rfind('/', imgPos);
            if (startKey == std::string::npos) startKey = lowerPath.rfind('\\', imgPos);

            std::string imgKey = lowerPath.substr(startKey + 1, imgPos + 4 - (startKey + 1));
            std::string fileName = lowerPath.substr(imgPos + 5);

            auto& candidates = g_WinningCandidates[imgKey];
            auto it = candidates.find(fileName);

            if (it == candidates.end()) {
                candidates[fileName] = { filePath, modPriority, fs::last_write_time(entry.path()), fs::file_size(entry.path()) };
            }
            else {
                bool replace = false;
                if (modPriority > it->second.priority) replace = true;
                else if (modPriority == it->second.priority && fs::last_write_time(entry.path()) < it->second.modTime) replace = true;

                if (replace) {
                    candidates[fileName] = { filePath, modPriority, fs::last_write_time(entry.path()), fs::file_size(entry.path()) };
                    Log("[PRIORITY] " + fileName + " in " + imgKey + " overridden by " + modName + " (Pri: " + std::to_string(modPriority) + ")");
                }
            }
        }
        else {
            std::string relativePath = fs::relative(entry.path(), modFolderPath).string();
            std::string relativeLower = ToLower(relativePath);

            auto it = g_LooseFileCandidates.find(relativeLower);
            if (it == g_LooseFileCandidates.end()) {
                g_LooseFileCandidates[relativeLower] = { filePath, modPriority, fs::last_write_time(entry.path()), fs::file_size(entry.path()) };
            }
            else {
                bool replace = false;
                if (modPriority > it->second.priority) replace = true;
                else if (modPriority == it->second.priority && fs::last_write_time(entry.path()) < it->second.modTime) replace = true;

                if (replace) {
                    g_LooseFileCandidates[relativeLower] = { filePath, modPriority, fs::last_write_time(entry.path()), fs::file_size(entry.path()) };
                    Log("[PRIORITY] Loose file '" + relativeLower + "' overridden by " + modName + " (Pri: " + std::to_string(modPriority) + ")");
                }
            }
        }
    }
}

void ScanAllMods() {
    if (!fs::exists(MODLOADER_DIR)) {
        fs::create_directories(MODLOADER_DIR);
        fs::create_directories(CUSTOM_DIR_FOLDER);
        fs::create_directories(DATA_DIR);
        return;
    }
    fs::create_directories(CUSTOM_DIR_FOLDER);
    fs::create_directories(DATA_DIR);

    for (const auto& entry : fs::directory_iterator(MODLOADER_DIR)) {
        if (!entry.is_directory()) continue;
        std::string folderName = entry.path().filename().string();
        if (ToLower(folderName) == ".dir" || ToLower(folderName) == ".data") continue;

        int priority = GetModPriority(folderName);
        Log("Scanning Mod Folder: " + folderName + " (Priority: " + std::to_string(priority) + ")");
        ScanSingleModFolder(entry.path().string(), priority);
    }
}

// ============================================================================
// PHASE 2: DIR GENERATION & REDIRECTION SETUP
// ============================================================================
#pragma pack(push, 1)
struct DirEntry32 { uint32_t offset; uint32_t size; char name[24]; };
#pragma pack(pop)

std::string FindOriginalDir(const std::string& imgKey) {
    std::string dirFileName = imgKey.substr(0, imgKey.length() - 4) + ".dir";
    std::vector<std::string> searchPaths = { "Stream/" + dirFileName, "stream/" + dirFileName, dirFileName, "Dat/" + dirFileName, "dat/" + dirFileName, "Objects/" + dirFileName, "Scripts/" + dirFileName, "Act/" + dirFileName };
    for (const auto& path : searchPaths) if (fs::exists(path)) return path;
    return "";
}

void ProcessElectionResults() {
    for (const auto& [imgKey, candidates] : g_WinningCandidates) {
        std::string originalDirPath = FindOriginalDir(imgKey);
        if (originalDirPath.empty()) continue;

        std::ifstream origDirFile(originalDirPath, std::ios::binary);
        std::vector<char> origDirData((std::istreambuf_iterator<char>(origDirFile)), std::istreambuf_iterator<char>());
        origDirFile.close();
        if (origDirData.size() % 32 != 0) continue;

        std::vector<DirEntry32> newDirEntries;
        std::map<std::string, DirEntry32*> originalEntriesMap;

        for (size_t i = 0; i < origDirData.size() / 32; i++) {
            DirEntry32* entry = reinterpret_cast<DirEntry32*>(origDirData.data() + i * 32);
            newDirEntries.push_back(*entry);
            std::string name(entry->name, strnlen(entry->name, 24));
            name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());
            originalEntriesMap[ToLower(name)] = &newDirEntries.back();
        }

        uint32_t nextFakeSector = FAKE_OFFSET_START;
        int stdCount = 0, virtCount = 0;

        for (const auto& [lowerName, candidate] : candidates) {
            uint32_t modSizeSectors = static_cast<uint32_t>((candidate.fileSize + SECTOR_SIZE - 1) / SECTOR_SIZE);
            auto it = originalEntriesMap.find(lowerName);

            if (it != originalEntriesMap.end()) {
                DirEntry32* origEntry = it->second;
                if (modSizeSectors <= origEntry->size) {
                    g_StandardReplacements[imgKey][origEntry->offset * SECTOR_SIZE] = { candidate.fullPath, origEntry->size * SECTOR_SIZE };
                    stdCount++;
                }
                else {
                    g_VirtualEntries[imgKey][nextFakeSector] = { lowerName, candidate.fullPath, modSizeSectors };
                    g_FakeOffsetLookup[nextFakeSector] = &g_VirtualEntries[imgKey][nextFakeSector];
                    origEntry->offset = nextFakeSector; origEntry->size = modSizeSectors;
                    nextFakeSector++; virtCount++;
                }
            }
            else {
                g_VirtualEntries[imgKey][nextFakeSector] = { lowerName, candidate.fullPath, modSizeSectors };
                g_FakeOffsetLookup[nextFakeSector] = &g_VirtualEntries[imgKey][nextFakeSector];
                DirEntry32 newEntry = { nextFakeSector, modSizeSectors, {} };
                strncpy(newEntry.name, lowerName.c_str(), 23); newEntry.name[23] = '\0';
                newDirEntries.push_back(newEntry);
                nextFakeSector++; virtCount++;
            }
        }

        Log("[" + imgKey + "] Finalized: " + std::to_string(stdCount) + " standard, " + std::to_string(virtCount) + " virtual.");

        if (virtCount > 0 || stdCount > 0) {
            std::string customDirPath = CUSTOM_DIR_FOLDER + imgKey.substr(0, imgKey.length() - 4) + ".dir";
            std::ofstream outDir(customDirPath, std::ios::binary);
            outDir.write(reinterpret_cast<const char*>(newDirEntries.data()), newDirEntries.size() * 32);
            outDir.close();
            g_DirRedirections[ToLower(originalDirPath)] = customDirPath;
        }
    }
}

// ============================================================================
// THE HOOKS (Pure VFS Focus)
// ============================================================================
std::string ResolveAndRedirectPath(const std::string& requestedPath) {
    char absPath[MAX_PATH];
    GetFullPathNameA(requestedPath.c_str(), MAX_PATH, absPath, NULL);
    std::string absLower = ToLower(absPath);

    if (absLower.find(g_ModloaderLower) != std::string::npos) return "";

    for (const auto& [orig, custom] : g_DirRedirections) {
        if (absLower.find(orig) != std::string::npos) return custom;
    }

    if (absLower.find(g_GameRootLower) == 0) {
        std::string relLower = absLower.substr(g_GameRootLower.length());
        auto it = g_LooseFileCandidates.find(relLower);
        if (it != g_LooseFileCandidates.end()) return it->second.fullPath;
    }
    return "";
}

HANDLE WINAPI HookedCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSec, DWORD dwDisp, DWORD dwFlags, HANDLE hTemp) {
    if (lpFileName) {
        std::string redirectedPath = ResolveAndRedirectPath(lpFileName);
        if (!redirectedPath.empty()) {
            HANDLE hFile = OriginalCreateFileA(redirectedPath.c_str(), dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
            if (hFile != INVALID_HANDLE_VALUE) {
                std::string key = ExtractIMGKey(redirectedPath);
                if (key.length() > 4 && key.substr(key.length() - 4) == ".img") {
                    EnterCriticalSection(&g_CriticalSection); g_IMGHandles[hFile] = key; LeaveCriticalSection(&g_CriticalSection);
                }
            }
            return hFile;
        }
        HANDLE hFile = OriginalCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
        if (hFile != INVALID_HANDLE_VALUE) {
            std::string key = ExtractIMGKey(lpFileName);
            if (key.length() > 4 && key.substr(key.length() - 4) == ".img") {
                EnterCriticalSection(&g_CriticalSection); g_IMGHandles[hFile] = key; LeaveCriticalSection(&g_CriticalSection);
            }
        }
        return hFile;
    }
    return OriginalCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
}

HANDLE WINAPI HookedCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSec, DWORD dwDisp, DWORD dwFlags, HANDLE hTemp) {
    if (lpFileName) {
        std::wstring wpath(lpFileName);
        std::string path(wpath.begin(), wpath.end());
        std::string redirectedPath = ResolveAndRedirectPath(path);
        if (!redirectedPath.empty()) {
            std::wstring wredirected(redirectedPath.begin(), redirectedPath.end());
            HANDLE hFile = OriginalCreateFileW(wredirected.c_str(), dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
            if (hFile != INVALID_HANDLE_VALUE) {
                std::string key = ExtractIMGKey(redirectedPath);
                if (key.length() > 4 && key.substr(key.length() - 4) == ".img") {
                    EnterCriticalSection(&g_CriticalSection); g_IMGHandles[hFile] = key; LeaveCriticalSection(&g_CriticalSection);
                }
            }
            return hFile;
        }
        HANDLE hFile = OriginalCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
        if (hFile != INVALID_HANDLE_VALUE) {
            std::string key = ExtractIMGKey(path);
            if (key.length() > 4 && key.substr(key.length() - 4) == ".img") {
                EnterCriticalSection(&g_CriticalSection); g_IMGHandles[hFile] = key; LeaveCriticalSection(&g_CriticalSection);
            }
        }
        return hFile;
    }
    return OriginalCreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
}

DWORD WINAPI HookedGetFileAttributesA(LPCSTR lpFileName) {
    if (lpFileName) {
        std::string redirectedPath = ResolveAndRedirectPath(lpFileName);
        if (!redirectedPath.empty()) return OriginalGetFileAttributesA(redirectedPath.c_str());
    }
    return OriginalGetFileAttributesA(lpFileName);
}

DWORD WINAPI HookedGetFileAttributesW(LPCWSTR lpFileName) {
    if (lpFileName) {
        std::wstring wpath(lpFileName);
        std::string path(wpath.begin(), wpath.end());
        std::string redirectedPath = ResolveAndRedirectPath(path);
        if (!redirectedPath.empty()) {
            std::wstring wredirected(redirectedPath.begin(), redirectedPath.end());
            return OriginalGetFileAttributesW(wredirected.c_str());
        }
    }
    return OriginalGetFileAttributesW(lpFileName);
}

DWORD WINAPI HookedSetFilePointer(HANDLE hFile, LONG lDist, PLONG pDistHigh, DWORD dwMethod) {
    DWORD res = OriginalSetFilePointer(hFile, lDist, pDistHigh, dwMethod);
    EnterCriticalSection(&g_CriticalSection);
    if (g_IMGHandles.count(hFile) && dwMethod == FILE_BEGIN) g_CurrentOffset[hFile] = res;
    LeaveCriticalSection(&g_CriticalSection);
    return res;
}

BOOL WINAPI HookedSetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDist, PLARGE_INTEGER pNew, DWORD dwMethod) {
    BOOL res = OriginalSetFilePointerEx(hFile, liDist, pNew, dwMethod);
    EnterCriticalSection(&g_CriticalSection);
    if (g_IMGHandles.count(hFile) && dwMethod == FILE_BEGIN && pNew) g_CurrentOffset[hFile] = pNew->LowPart;
    LeaveCriticalSection(&g_CriticalSection);
    return res;
}

DWORD WINAPI HookedGetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    EnterCriticalSection(&g_CriticalSection); bool isIMG = g_IMGHandles.count(hFile) > 0; LeaveCriticalSection(&g_CriticalSection);
    if (isIMG) { if (lpFileSizeHigh) *lpFileSizeHigh = 0; return 0x7FFFFFFF; }
    return OriginalGetFileSize(hFile, lpFileSizeHigh);
}

BOOL WINAPI HookedGetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    EnterCriticalSection(&g_CriticalSection); bool isIMG = g_IMGHandles.count(hFile) > 0; LeaveCriticalSection(&g_CriticalSection);
    if (isIMG) { lpFileSize->QuadPart = 0x7FFFFFFF; return TRUE; }
    return OriginalGetFileSizeEx(hFile, lpFileSize);
}

BOOL WINAPI HookedReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nBytesToRead, LPDWORD pBytesRead, LPOVERLAPPED lpOver) {
    EnterCriticalSection(&g_CriticalSection);
    auto it = g_IMGHandles.find(hFile);
    if (it != g_IMGHandles.end()) {
        std::string imgKey = it->second;
        uint32_t offset = lpOver ? lpOver->Offset : g_CurrentOffset[hFile];
        uint32_t sectorOffset = offset / SECTOR_SIZE;

        auto virtIt = g_FakeOffsetLookup.find(sectorOffset);
        if (virtIt != g_FakeOffsetLookup.end()) {
            VirtualEntry* vEntry = virtIt->second;
            std::ifstream modFile(vEntry->modPath, std::ios::binary);
            if (modFile.is_open()) {
                memset(lpBuffer, 0, nBytesToRead);
                modFile.read(static_cast<char*>(lpBuffer), nBytesToRead);
                DWORD bytesRead = static_cast<DWORD>(modFile.gcount());
                if (pBytesRead) *pBytesRead = bytesRead;
                if (lpOver && lpOver->hEvent) SetEvent(lpOver->hEvent);
                LeaveCriticalSection(&g_CriticalSection); return TRUE;
            }
        }

        auto stdIt = g_StandardReplacements.find(imgKey);
        if (stdIt != g_StandardReplacements.end()) {
            auto repIt = stdIt->second.find(offset);
            if (repIt != stdIt->second.end()) {
                StandardReplacement* rep = &repIt->second;
                std::ifstream modFile(rep->modPath, std::ios::binary);
                if (modFile.is_open()) {
                    memset(lpBuffer, 0, nBytesToRead);
                    modFile.read(static_cast<char*>(lpBuffer), nBytesToRead);
                    DWORD bytesRead = static_cast<DWORD>(modFile.gcount());
                    if (pBytesRead) *pBytesRead = bytesRead;
                    if (lpOver && lpOver->hEvent) SetEvent(lpOver->hEvent);
                    LeaveCriticalSection(&g_CriticalSection); return TRUE;
                }
            }
        }
    }
    LeaveCriticalSection(&g_CriticalSection);
    return OriginalReadFile(hFile, lpBuffer, nBytesToRead, pBytesRead, lpOver);
}

// ============================================================================
// INITIALIZATION
// ============================================================================
void InitializeHooks() {
    InitializeCriticalSection(&g_CriticalSection);

    // NEW: Load settings BEFORE doing anything else
    LoadSettings();

    // Only clear the log file if logging is enabled
    if (g_EnableLogging) {
        std::ofstream(LOG_FILE, std::ios::trunc).close();
    }

    Log("Bully Modloader (Pure VFS Edition) initialized.");
    Log("Logging is currently: " + std::string(g_EnableLogging ? "ENABLED" : "DISABLED"));

    InitPaths();
    ScanAllMods();
    ProcessElectionResults();

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

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitializeHooks();
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        MH_Uninitialize();
        DeleteCriticalSection(&g_CriticalSection);
    }
    return TRUE;
}