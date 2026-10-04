#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <algorithm>
#include <map>
#include <set>
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
const uint32_t MAX_FAKE_SECTOR = 0x7FFFFFFF / SECTOR_SIZE;

CRITICAL_SECTION g_CriticalSection;
bool g_EnableLogging = true;

struct FileCandidate {
    std::string modPath;
    int priority;
    fs::file_time_type modTime;
    uintmax_t fileSize;
};

struct LooseFileCandidate {
    std::string modPath;
    int priority;
    fs::file_time_type modTime;
};

struct StandardReplacement { std::string modPath; uint64_t originalSize; uint64_t modSize; };
struct VirtualEntry { std::string name; std::string modPath; uint32_t sizeInSectors; uint64_t modSize; };

std::map<std::string, std::map<uint64_t, StandardReplacement>> g_StandardReplacements;
std::map<std::string, std::map<uint32_t, VirtualEntry>> g_VirtualEntries;
std::map<std::string, std::string> g_OriginalDirCache;
std::map<std::string, std::string> g_DirRedirections;
std::map<std::string, LooseFileCandidate> g_LooseFiles; // NEW: For loose file replacements
std::map<HANDLE, std::string> g_IMGHandles;
std::map<HANDLE, std::string> g_IMGPaths;
std::map<HANDLE, uint64_t> g_CurrentOffset;
std::set<std::string> g_LoggedReplacementReads;
struct CompletionPortAssociation { HANDLE port; ULONG_PTR completionKey; };
std::map<HANDLE, CompletionPortAssociation> g_CompletionPortAssociations;

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
typedef HANDLE(WINAPI* OrigCreateIoCompletionPort_t)(HANDLE, HANDLE, ULONG_PTR, DWORD);
OrigCreateIoCompletionPort_t OriginalCreateIoCompletionPort = nullptr;

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
// SCANNING & DIR GENERATION
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
    if (baseName.length() <= 4) return "";
    std::string dirFileName = baseName.substr(0, baseName.length() - 4) + ".dir";
    std::string cacheKey = ToLower(dirFileName);
    auto cachedDir = g_OriginalDirCache.find(cacheKey);
    if (cachedDir != g_OriginalDirCache.end()) return cachedDir->second;

    // Expanded search paths to include UI, Fonts, Menus, etc.
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
    for (const auto& p : searchPaths) {
        fs::path candidate = fs::path(g_GameRoot) / p;
        if (fs::exists(candidate)) {
            g_OriginalDirCache[cacheKey] = p;
            return p;
        }
    }

    std::error_code error;
    fs::recursive_directory_iterator it(
        g_GameRoot,
        fs::directory_options::skip_permission_denied,
        error);
    const fs::recursive_directory_iterator end;
    while (!error && it != end) {
        const fs::path currentPath = it->path();
        std::string relativePath = ToLower(fs::relative(currentPath, g_GameRoot, error).string());
        if (error) {
            error.clear();
            it.increment(error);
            continue;
        }
        if (relativePath == "modloader" || relativePath.find("modloader/") == 0) {
            if (it->is_directory(error)) it.disable_recursion_pending();
            error.clear();
        }
        else if (it->is_regular_file(error) && ToLower(currentPath.filename().string()) == cacheKey) {
            std::string result = fs::relative(currentPath, g_GameRoot, error).string();
            if (!error) {
                g_OriginalDirCache[cacheKey] = result;
                return result;
            }
            error.clear();
        }
        it.increment(error);
    }
    g_OriginalDirCache[cacheKey] = "";
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
            // Check if it's inside an .img folder
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
            // NEW: Otherwise, treat it as a loose file replacement
            else {
                std::string relPath = fs::relative(file.path(), entry.path()).string();
                std::string relLower = ToLower(relPath);

                auto it = g_LooseFiles.find(relLower);
                bool shouldReplaceLoose = false;

                if (it == g_LooseFiles.end()) {
                    shouldReplaceLoose = true;
                }
                else if (priority > it->second.priority) {
                    shouldReplaceLoose = true;
                }
                else if (priority == it->second.priority && file.last_write_time() < it->second.modTime) {
                    shouldReplaceLoose = true;
                }

                if (shouldReplaceLoose) {
                    g_LooseFiles[relLower] = { file.path().string(), priority, file.last_write_time() };
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

        std::ifstream origDirFile(fs::path(g_GameRoot) / originalDirPath, std::ios::binary);
        if (!origDirFile.is_open()) {
            Log("ERROR: Unable to open archive directory: " + originalDirPath);
            continue;
        }
        std::vector<char> origDirData((std::istreambuf_iterator<char>(origDirFile)), std::istreambuf_iterator<char>());
        origDirFile.close();
        if (origDirData.size() % 32 != 0) {
            Log("ERROR: Invalid archive directory size: " + originalDirPath);
            continue;
        }

        std::vector<DirEntry32> newDirEntries;
        newDirEntries.reserve(origDirData.size() / 32 + imgPair.second.size());
        std::map<std::string, DirEntry32*> origMap;

        for (size_t i = 0; i < origDirData.size() / 32; i++) {
            DirEntry32* e = reinterpret_cast<DirEntry32*>(origDirData.data() + i * 32);
            newDirEntries.push_back(*e);
            std::string name(e->name, strnlen(e->name, 24));
            name.erase(std::remove(name.begin(), name.end(), '\0'), name.end());
            origMap[ToLower(name)] = &newDirEntries.back();
        }

        uint64_t nextFakeSector = FAKE_OFFSET_START;
        int stdCount = 0, virtCount = 0;

        for (const auto& filePair : imgPair.second) {
            const std::string& lowerName = filePair.first;
            const FileCandidate& data = filePair.second;
            uint64_t modSizeSectors64 = data.fileSize / SECTOR_SIZE + (data.fileSize % SECTOR_SIZE != 0);
            if (modSizeSectors64 > UINT32_MAX) {
                Log("Skipping oversized mod file [" + imgKey + "]: " + lowerName);
                continue;
            }
            uint32_t modSizeSectors = static_cast<uint32_t>(std::max<uint64_t>(modSizeSectors64, 1));
            auto it = origMap.find(lowerName);

            if (it != origMap.end()) {
                DirEntry32* origEntry = it->second;
                if (modSizeSectors <= origEntry->size) {
                    g_StandardReplacements[imgKey][static_cast<uint64_t>(origEntry->offset) * SECTOR_SIZE] = {
                        data.modPath,
                        static_cast<uint64_t>(origEntry->size) * SECTOR_SIZE,
                        data.fileSize
                    };
                    stdCount++;
                }
                else {
                    if (nextFakeSector + modSizeSectors - 1 > MAX_FAKE_SECTOR) {
                        Log("Skipping oversized virtual entry [" + imgKey + "]: " + lowerName);
                        continue;
                    }
                    uint32_t fakeSector = static_cast<uint32_t>(nextFakeSector);
                    g_VirtualEntries[imgKey][fakeSector] = { lowerName, data.modPath, modSizeSectors, data.fileSize };
                    origEntry->offset = fakeSector; origEntry->size = modSizeSectors;
                    nextFakeSector += modSizeSectors; virtCount++;
                }
            }
            else {
                if (nextFakeSector + modSizeSectors - 1 > MAX_FAKE_SECTOR) {
                    Log("Skipping oversized virtual entry [" + imgKey + "]: " + lowerName);
                    continue;
                }
                uint32_t fakeSector = static_cast<uint32_t>(nextFakeSector);
                g_VirtualEntries[imgKey][fakeSector] = { lowerName, data.modPath, modSizeSectors, data.fileSize };
                DirEntry32 newEntry = { fakeSector, modSizeSectors, {} };
                strncpy(newEntry.name, lowerName.c_str(), sizeof(newEntry.name));
                newDirEntries.push_back(newEntry);
                nextFakeSector += modSizeSectors; virtCount++;
            }
        }

        Log("[" + imgKey + "] Finalized: " + std::to_string(stdCount) + " standard, " + std::to_string(virtCount) + " virtual.");

        std::string customDirPath = CUSTOM_DIR_FOLDER + originalDirPath;
        fs::create_directories(fs::path(customDirPath).parent_path());
        std::ofstream outDir(customDirPath, std::ios::binary);
        if (outDir.is_open()) {
            outDir.write(reinterpret_cast<const char*>(newDirEntries.data()), newDirEntries.size() * 32);
            outDir.close();
            if (outDir) g_DirRedirections[ToLower(originalDirPath)] = customDirPath;
            else Log("ERROR: Unable to write generated archive directory: " + customDirPath);
        }
        else Log("ERROR: Unable to create generated archive directory: " + customDirPath);
    }
}

// ============================================================================
// THE HOOKS
// ============================================================================
std::string ResolveAndRedirectPath(const std::string& requestedPath) {
    char absPath[MAX_PATH];
    GetFullPathNameA(requestedPath.c_str(), MAX_PATH, absPath, NULL);
    std::string absLower = ToLower(absPath);

    if (absLower.find(g_ModloaderLower) != std::string::npos) return "";

    for (const auto& pair : g_DirRedirections) {
        if (absLower.find(pair.first) != std::string::npos) return pair.second;
    }

    // NEW: Check if it's a loose file in the game root
    if (absLower.find(g_GameRootLower) == 0) {
        std::string relLower = absLower.substr(g_GameRootLower.length());
        auto it = g_LooseFiles.find(relLower);
        if (it != g_LooseFiles.end()) {
            return it->second.modPath;
        }
    }

    return "";
}

HANDLE WINAPI HookedCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSec, DWORD dwDisp, DWORD dwFlags, HANDLE hTemp) {
    if (!lpFileName) return OriginalCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
    std::string r = ResolveAndRedirectPath(lpFileName);
    HANDLE hFile = OriginalCreateFileA(r.empty() ? lpFileName : r.c_str(), dwDesiredAccess, dwShareMode, lpSec, dwDisp, dwFlags, hTemp);
    if (hFile != INVALID_HANDLE_VALUE) {
        std::string key = ExtractIMGKey(r.empty() ? lpFileName : r);
        if (key.length() > 4 && key.substr(key.length() - 4) == ".img") {
            EnterCriticalSection(&g_CriticalSection);
            g_IMGHandles[hFile] = key;
            g_IMGPaths[hFile] = r.empty() ? lpFileName : r;
            g_CurrentOffset[hFile] = 0;
            LeaveCriticalSection(&g_CriticalSection);
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
            EnterCriticalSection(&g_CriticalSection);
            g_IMGHandles[hFile] = key;
            g_IMGPaths[hFile] = r.empty() ? path : r;
            g_CurrentOffset[hFile] = 0;
            LeaveCriticalSection(&g_CriticalSection);
        }
    }
    return hFile;
}

DWORD WINAPI HookedSetFilePointer(HANDLE hFile, LONG lDist, PLONG pDistHigh, DWORD dwMethod) {
    DWORD res = OriginalSetFilePointer(hFile, lDist, pDistHigh, dwMethod);
    DWORD error = GetLastError();
    if (res != INVALID_SET_FILE_POINTER || error == NO_ERROR) {
        uint64_t offset = static_cast<uint64_t>(res);
        if (pDistHigh) offset |= static_cast<uint64_t>(static_cast<uint32_t>(*pDistHigh)) << 32;
        EnterCriticalSection(&g_CriticalSection);
        if (g_IMGHandles.count(hFile)) g_CurrentOffset[hFile] = offset;
        LeaveCriticalSection(&g_CriticalSection);
    }
    return res;
}

BOOL WINAPI HookedSetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDist, PLARGE_INTEGER pNew, DWORD dwMethod) {
    BOOL res = OriginalSetFilePointerEx(hFile, liDist, pNew, dwMethod);
    if (res && pNew) { EnterCriticalSection(&g_CriticalSection); if (g_IMGHandles.count(hFile)) g_CurrentOffset[hFile] = static_cast<uint64_t>(pNew->QuadPart); LeaveCriticalSection(&g_CriticalSection); }
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
    std::string archiveKey;
    std::string archivePath;
    CompletionPortAssociation completionPort = {};
    bool hasCompletionPort = false;
    uint64_t offset = 0;
    uint64_t requestEnd = 0;
    struct ReplacementRange {
        uint64_t start;
        uint64_t end;
        std::string modPath;
        uint64_t modSize;
    };
    std::vector<ReplacementRange> replacements;

    EnterCriticalSection(&g_CriticalSection);
    auto it = g_IMGHandles.find(hFile);
    if (it != g_IMGHandles.end()) {
        archiveKey = it->second;
        auto pathIt = g_IMGPaths.find(hFile);
        if (pathIt != g_IMGPaths.end()) archivePath = pathIt->second;
        auto completionIt = g_CompletionPortAssociations.find(hFile);
        if (completionIt != g_CompletionPortAssociations.end()) {
            completionPort = completionIt->second;
            hasCompletionPort = true;
        }
        offset = lpOver
            ? (static_cast<uint64_t>(lpOver->OffsetHigh) << 32) | lpOver->Offset
            : g_CurrentOffset[hFile];

        requestEnd = offset + nBytesToRead;
        if (requestEnd < offset) requestEnd = UINT64_MAX;
        auto standardEntries = g_StandardReplacements.find(archiveKey);
        if (standardEntries != g_StandardReplacements.end()) {
            auto entry = standardEntries->second.lower_bound(offset);
            if (entry != standardEntries->second.begin()) --entry;
            for (; entry != standardEntries->second.end() && entry->first < requestEnd; ++entry) {
                uint64_t end = entry->first + entry->second.originalSize;
                if (end > offset) {
                    replacements.push_back({ entry->first, end, entry->second.modPath, entry->second.modSize });
                }
            }
        }
        auto virtualEntries = g_VirtualEntries.find(archiveKey);
        if (virtualEntries != g_VirtualEntries.end()) {
            for (const auto& entry : virtualEntries->second) {
                uint64_t start = static_cast<uint64_t>(entry.first) * SECTOR_SIZE;
                uint64_t end = start + static_cast<uint64_t>(entry.second.sizeInSectors) * SECTOR_SIZE;
                if (start < requestEnd && end > offset) {
                    replacements.push_back({ start, end, entry.second.modPath, entry.second.modSize });
                }
            }
        }
    }
    LeaveCriticalSection(&g_CriticalSection);

    if (!replacements.empty()) {
        if (nBytesToRead > 0 && !lpBuffer) {
            SetLastError(ERROR_INVALID_USER_BUFFER);
            return FALSE;
        }
        if (archivePath.empty()) {
            Log("ERROR: Missing backing archive path for replacement read [" + archiveKey + "].");
            SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
        HANDLE backingFile = OriginalCreateFileA(
            archivePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (backingFile == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            Log("ERROR: Unable to open backing archive for replacement read: " + archivePath);
            SetLastError(error);
            return FALSE;
        }
        if (nBytesToRead > 0) memset(lpBuffer, 0, nBytesToRead);
        LARGE_INTEGER archiveOffset;
        archiveOffset.QuadPart = static_cast<LONGLONG>(offset);
        BOOL readResult = OriginalSetFilePointerEx(backingFile, archiveOffset, nullptr, FILE_BEGIN);
        DWORD backingBytesRead = 0;
        if (readResult) {
            readResult = OriginalReadFile(backingFile, lpBuffer, nBytesToRead, &backingBytesRead, nullptr);
        }
        DWORD readError = readResult ? NO_ERROR : GetLastError();
        CloseHandle(backingFile);
        if (!readResult) {
            Log("ERROR: Unable to read backing archive for replacement read: " + archivePath);
            SetLastError(readError);
            return FALSE;
        }

        for (const ReplacementRange& replacement : replacements) {
            uint64_t overlapStart = (std::max)(offset, replacement.start);
            uint64_t overlapEnd = (std::min)(requestEnd, replacement.end);
            if (overlapStart >= overlapEnd) continue;
            uint64_t overlapLength = overlapEnd - overlapStart;
            DWORD targetOffset = static_cast<DWORD>(overlapStart - offset);
            DWORD targetLength = static_cast<DWORD>(overlapLength);
            memset(static_cast<char*>(lpBuffer) + targetOffset, 0, targetLength);
            if (overlapStart >= replacement.start &&
                overlapStart - replacement.start < replacement.modSize) {
                uint64_t sourceOffset = overlapStart - replacement.start;
                DWORD sourceLength = static_cast<DWORD>(
                    std::min<uint64_t>(overlapLength, replacement.modSize - sourceOffset));
                std::ifstream modFile(replacement.modPath, std::ios::binary);
                if (!modFile.is_open()) {
                    Log("ERROR: Unable to open replacement file: " + replacement.modPath);
                    SetLastError(ERROR_FILE_NOT_FOUND);
                    return FALSE;
                }
                modFile.seekg(static_cast<std::streamoff>(sourceOffset));
                if (!modFile) {
                    Log("ERROR: Unable to seek replacement file: " + replacement.modPath);
                    SetLastError(ERROR_READ_FAULT);
                    return FALSE;
                }
                modFile.read(static_cast<char*>(lpBuffer) + targetOffset, sourceLength);
                if (static_cast<DWORD>(modFile.gcount()) != sourceLength || modFile.bad()) {
                    Log("ERROR: Replacement file changed or was truncated while reading: " + replacement.modPath);
                    SetLastError(ERROR_HANDLE_EOF);
                    return FALSE;
                }
            }

            std::string readKey = archiveKey + "|" + replacement.modPath;
            bool logRead = false;
            EnterCriticalSection(&g_CriticalSection);
            logRead = g_LoggedReplacementReads.insert(readKey).second;
            LeaveCriticalSection(&g_CriticalSection);
            if (logRead) {
                Log("Replacement read [" + archiveKey + "]: " + replacement.modPath +
                    " at archive offset " + std::to_string(offset) +
                    " (replacement range " + std::to_string(replacement.start) + "-" +
                    std::to_string(replacement.end) + ", requested " +
                    std::to_string(nBytesToRead) + ", source " +
                    std::to_string(replacement.modSize) + ", " +
                    (lpOver ? "overlapped" : "synchronous") + ").");
            }
        }

        DWORD bytesRead = nBytesToRead;
        if (pBytesRead) *pBytesRead = bytesRead;
        if (lpOver) {
            lpOver->Internal = 0;
            lpOver->InternalHigh = bytesRead;
            ULONG_PTR eventValue = reinterpret_cast<ULONG_PTR>(lpOver->hEvent);
            HANDLE eventHandle = reinterpret_cast<HANDLE>(eventValue & ~static_cast<ULONG_PTR>(1));
            bool suppressCompletionPort = (eventValue & 1) != 0;
            if (eventHandle && !SetEvent(eventHandle)) {
                DWORD error = GetLastError();
                Log("ERROR: Unable to signal overlapped replacement read.");
                SetLastError(error);
                return FALSE;
            }
            if (hasCompletionPort && !suppressCompletionPort &&
                !PostQueuedCompletionStatus(completionPort.port, bytesRead, completionPort.completionKey, lpOver)) {
                DWORD error = GetLastError();
                Log("ERROR: Unable to queue overlapped replacement completion.");
                SetLastError(error);
                return FALSE;
            }
        }
        else {
            LARGE_INTEGER nextOffset;
            nextOffset.QuadPart = static_cast<LONGLONG>(offset + bytesRead);
            OriginalSetFilePointerEx(hFile, nextOffset, nullptr, FILE_BEGIN);
            EnterCriticalSection(&g_CriticalSection);
            if (g_IMGHandles.count(hFile)) g_CurrentOffset[hFile] = offset + bytesRead;
            LeaveCriticalSection(&g_CriticalSection);
        }
        return TRUE;
    }
    DWORD bytesRead = 0;
    LPDWORD bytesReadPointer = pBytesRead ? pBytesRead : &bytesRead;
    BOOL result = OriginalReadFile(hFile, lpBuffer, nBytesToRead, bytesReadPointer, lpOver);
    if (result && !lpOver) {
        EnterCriticalSection(&g_CriticalSection);
        if (g_IMGHandles.count(hFile)) g_CurrentOffset[hFile] = offset + *bytesReadPointer;
        LeaveCriticalSection(&g_CriticalSection);
    }
    return result;
}

HANDLE WINAPI HookedCreateIoCompletionPort(
    HANDLE hFile,
    HANDLE hExistingCompletionPort,
    ULONG_PTR dwCompletionKey,
    DWORD dwNumberOfConcurrentThreads) {
    HANDLE completionPort = OriginalCreateIoCompletionPort(
        hFile,
        hExistingCompletionPort,
        dwCompletionKey,
        dwNumberOfConcurrentThreads);
    if (completionPort && hFile != INVALID_HANDLE_VALUE) {
        EnterCriticalSection(&g_CriticalSection);
        g_CompletionPortAssociations[hFile] = { completionPort, dwCompletionKey };
        LeaveCriticalSection(&g_CriticalSection);
    }
    return completionPort;
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
    MH_CreateHook(&CreateIoCompletionPort, &HookedCreateIoCompletionPort, (LPVOID*)&OriginalCreateIoCompletionPort); MH_EnableHook(&CreateIoCompletionPort);

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