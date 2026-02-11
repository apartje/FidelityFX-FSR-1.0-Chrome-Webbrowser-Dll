# FidelityFX Super Resolution 1.0 for Chrome  
(DLL Injection Mod)

This project brings AMD FidelityFX Super Resolution 1.0 (FSR) upscaling and RCAS sharpening to Google Chrome / Chromium-based browsers using DLL injection.

The goal is sharper and cleaner video playback with minimal performance overhead.

---

#known bugs
not working on: Opera/firefox
hotkey toggle doesnt work sometimes

## Features

- FSR 1.0 upscaling  
- RCAS sharpening  
- Works on video playback in Chrome  
- Toggle on/off at runtime  
- Visual confirmation when injection is active  

---

## Controls

| Action | Key |
|------|-----|
| Toggle FSR | F10 |

---

## Requirements & Limitations

- Hardware acceleration must be enabled  
- Software rendering is NOT supported  

---

## Installation

### Copy Shader Files

Copy the following files:

- rcas.hlsl  
- fsr1.hlsl  
- ffx_a.h  
- ffx_fsr1.h  

into your Chrome installation directory:

C:\Program Files\Google\Chrome\Application%VERSION%


Example:

C:\Program Files\Google\Chrome\Application\144.0.7559.133


---

### Optional: Automatic Loading (No Injector Required)

If a dxil.dll already exists in the Chrome folder:

- Replace it with the provided DLL  
- The file must be named dxil.dll  

This allows the mod to load automatically when Chrome starts.

---

## Manual Injection

If you do not replace dxil.dll:

Inject the DLL into the Chrome process with the window title:

Chrome_WidgetWin_0


If the injection is successful, a green 20×20 pixel square will appear on every video as a visual indicator.

---

## Video Demonstration

https://www.youtube.com/watch?v=7PVtNNtjnyE

---

## Resources

### Official AMD FidelityFX FSR 1.0 shaders  
https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/master/ffx-fsr

### MinHook hde64 files  
https://github.com/TsudaKageyu/minhook/tree/master/src/hde

---

## Comparison

### Without FSR
![Without FSR](nonfsr.PNG)

### With FSR
![With FSR](fsr.PNG)
