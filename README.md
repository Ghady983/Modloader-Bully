# Bully Modloader

A non-destructive Virtual File System (VFS) modloader for **Bully: Scholarship Edition**. Inspired by [thelink2012's GTA Modloader](https://github.com/thelink2012/modloader).

Replace, resize, and add files inside `.img` archives and the game's root directory — **without ever modifying the original game files**. Simply drop your mods into organized folders, set priorities, and launch the game. Removing a mod is as easy as deleting its folder.

---

## ✨ Features

- **100% Non-Destructive**: Original `.img`, `.dir`, and loose files are never modified. Remove a mod folder and the game reverts to vanilla instantly.
- **Archive Virtualization**: Replace or add files inside any `.img` archive (e.g., `Stream/world.img`) by simply mirroring the folder structure.
- **Larger Files & New Additions**: Automatically generates a custom `.dir` file when your replacement is larger than the original or is a brand-new file. No manual IMG editing required.
- **Root Directory Emulation**: Each mod folder acts as a virtual game root. Drop any loose file (`.ini`, `.col`, `.ipl`, etc.) and it transparently replaces the original.
- **Priority System**: Control load order via `priority.ini`. Higher number = higher priority. Equal priorities fall back to the earliest "Date Modified".
- **Toggleable Debug Logging**: Enable or disable the log file via `modloader.ini` to keep your game directory clean.
- **Thread-Safe**: All hooks use a critical section to prevent race conditions during Bully's asynchronous streaming.

---

## 📥 Installation

### Prerequisites
- **Bully: Scholarship Edition** (PC, version 1.200 — Steam/GOG default)
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (`dinput8.dll`)

### Steps
1. Place `dinput8.dll` in your Bully game directory (where `Bully.exe` is).
2. Place **`modloader_bully.asi`** in the same directory.
3. Create a folder named `modloader` in the game directory.
4. Launch the game. The modloader will auto-generate its config files on first run.

---

## 🔧 Known Bugs 

1. Sometimes some mods might not work or the game might crash : to solve this try deleting the `.dir` folder in `modloader` folder.

2. Upon updating the modloader , sometimes you might wanna delete the `.data` folder too (since I would have added modifications to it) but note this removes the priority list you made in the previous version

3. This is a possible perceived bug I found , if you're making an `.nft` (Texture) modification : it will not show any changes if you don't also provide the `.nif` (3d Model) with it. And the opposite goes to if you are making an `.nif` mod , you must provide also the `.nft` file with it or else no texture will show.
That's why the mod works best with `.nif + .nft` combination

---

## 📂 Usage & Folder Structure

```text
Bully/
├── Bully.exe
├── dinput8.dll
├── modloader_bully.asi      <-- The modloader
└── modloader/
    ├── modloader.ini        <-- Global settings (Debug toggle)
    ├── .data/
    │   └── mods.ini     <-- Mod load order and status of each mod
    ├── .dir/
        └── Stream/
    │       └── world.dir    <-- Auto-generated custom .dir (do not edit)
    ├── MyMod/               <-- A mod folder (acts as virtual game root)
    │   └── Stream/
    │       └── world.img/
    │           ├── X.nif
    │           └── X.nft
    ├── MyOtherMod/          <-- Another mod folder
    │   └── Stream/
    │       └── world.img/
    │           ├── Y.nif
    │           └── Y.nft
    ├── ...
```

### Replacing a File Inside an `.img`
1. Find the file you want to replace (e.g., `foreign.nif` inside `Stream/world.img`).
2. Create a mod folder: `modloader/MyMod/Stream/world.img/`.
3. Place your replacement `foreign.nif` inside that folder.
4. Launch the game. Done.

### Adding a New File to an `.img`
Follow the same steps as above. If the filename doesn't exist in the original `.dir`, the modloader automatically appends it to a generated custom `.dir` file.

---

## ⚙️ Configuration

### Mod Priority
Open `modloader/.data/priority.ini`:
```ini
[Priorities]
MyTextureMod=50
MyConfigMod=60
```
- **Higher number = wins conflicts.**
- **Equal numbers = earliest "Date Modified" wins.**
- New mod folders are auto-added with a default priority of `50`.

### Disabling the Log
Open `modloader/modloader.ini`:
```ini
[Settings]
Debug=0
```
Set to `1` to enable `modloader.txt` logging. Set to `0` to disable it and save disk I/O.

---

## 🛠️ How It Works (Technical)

| Phase | What Happens |
|-------|-------------|
| **Startup** | Reads `modloader.ini` for settings. Recursively scans all mod folders. Resolves conflicts via priority/date. |
| **`.dir` Generation** | If any file is larger than the original or is a new addition, a custom `.dir` is written to `modloader/.dir/`. |
| **Hooking** | Hooks `CreateFileA/W`, `ReadFile`, `SetFilePointer/Ex`, `GetFileSize/Ex`, and `GetFileAttributesA/W` via MinHook. |
| **Runtime – Loose Files** | When the game requests a file, the hook checks if a mod folder provides a replacement. If yes, the path is silently redirected. |
| **Runtime – `.img` Files** | When the game seeks to an offset inside an `.img`, the hook checks if that offset maps to a modded file. If yes, data is read from the mod folder instead. |
| **Runtime – Fake Offsets** | For larger/new files, the custom `.dir` points to a "fake" sector offset (starting at 1GB) beyond the real `.img` size. The hook intercepts reads at that offset and serves the mod file. `GetFileSize` is spoofed to prevent "file too small" crashes. |

---

## 🏗️ Building from Source

### Requirements
- Visual Studio 2019/2022 (C++ Desktop workload)
- [MinHook](https://github.com/TsudaKageyu/minhook) (add source files directly to the project)
- Platform: **x86 (32-bit)** — Bully is a 32-bit game
- Configuration: **Release**

### Steps
1. Open `Modloader-Bully/Modloader-Bully.slnx`.
2. Right Click Project in Solution Explorer -> Go To Properties -> Configuration Manager => Set platform to **x86** and configuration to **Release**.
3. Disable Precompiled Headers if it's not already disabled: *Project Properties → C/C++ → Precompiled Headers → Not Using Precompiled Headers*.
4. Add all MinHook `.c` source files (`src/*.c` and `src/hde/*.c`) to the project if they're not already added. (Right Click Project-> Add -> Existing Item)
5. Add MinHook's `include/` folder to *Additional Include Directories*. `(Right Click Project -> Properties -> C/C++ -> General)`
6. Change Target File Extension to .asi `(Properties -> Configuration Properties -> Advanced -> Target File Extension)`
7. Change Target Name to "modloader_bully" `(Properties -> Configuration Properties -> General -> Target Name)`
8. To Build , Go Build (from Top Bar) / Clean Solution then Build/Rebuild Solution
9. You Will Find the .asi in the Release Folder, Drop it to the root directory of Bully

---

## 🤝 Credits & Inspiration

- [thelink2012 – GTA Modloader](https://github.com/thelink2012/modloader): Architectural inspiration for the VFS approach.
- [TsudaKageyu – MinHook](https://github.com/TsudaKageyu/minhook): The inline hooking library that makes this possible.
- [ThirteenAG – Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader): ASI injection.
- [CookiePLMonster – SilentPatchBully](https://github.com/CookiePLMonster/SilentPatchBully): Reference for Bully's internal memory layout and RenderWare structures.
- The Bully modding community.

---

## 📌 Note

This project is still in Development , So you might expect some bugs. I am still new at modding Bully and I used AI to help me build this system, I just thought about sharing this system because it helped me manage my own Bully Mods.

---

## ⚖️ License

This project is provided as-is for educational and personal modding purposes. Bully: Scholarship Edition is © Rockstar Games.

---
[![Donate on Patreon](https://img.shields.io/badge/Patreon-Donate-F96854?style=for-the-badge&logo=patreon&logoColor=white)](https://patreon.com/ghady983)