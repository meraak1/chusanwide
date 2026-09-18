# chusanwide

Widens the highway to align with your controller without needing
a 32 inch monitor. This stretches the actual 3D playfield rather than
just resizing the window (so all UI stays on screen).

## Install

1. Download `chusanwide.dll` from releases and put it next to `launch.bat`

2. Add `-k chusanwide.dll` to `launch.bat`:

```
inject_x86 -d -k chusanhook_x86.dll -k chusanwide.dll chusanApp.exe
```

3. `chusanwide.ini` is automatically created next to the DLL on the first launch

4. Edit the .ini to change the `sx` value to align it with your
controller. 

For a 27 inch monitor, set `sx` to 1.165 (default). For 25 inch, set to 1.26.
For 24 inch, set to 1.31. If you have an unusual screen size / controller size,
calculate `sx = controller size (inches) / screen size (inches)`.
You can also use the left/right arrow keys in game to
slowly nudge the size of the highway until it is correctly aligned.


This works in fullscreen and windowed.


## Building

**Windows**: Visual Studio with the Desktop C++ workload, CMake, Git:

```
cmake -B build -A Win32
cmake --build build --config Release
```

**Linux**

```
sudo apt install mingw-w64 cmake git
./build.sh
```


