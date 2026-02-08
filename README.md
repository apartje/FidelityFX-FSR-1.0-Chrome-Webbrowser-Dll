FidelityFX Super Resolution 1.0 (FSR) upscaling and sharpening for Chrome/web browsers via DLL injection.
enable/disable: press F10
Video: https://youtu.be/XUDsCbMS07Y

Place all files into(rcas.hlsl,fsr1.hlsl,ffx_a.h,ffx_fsr1.h):
C:\Program Files\Google\Chrome\Application\%VERSION%
(e.g. 144.0.7559.133) u can also if u have a dxil.dll inside the chrome folder replace it with our dll(the dll should also be named dxil.dll)
to make it work automatilcy without injectin

Then inject the DLL into the Chrome process with the window title "Chrome_WidgetWin_0".

If the injection is successful, a green 20×20 pixel box will appear on every video.

Download for the FSR 1.0 Shaders: https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/master/ffx-fsr
