# printer_fork

![logo](printer_fork.png)

Tiny Windows launcher for `.3mf` files. Set it as the default app for `.3mf` and it forwards the file to the right slicer:

- **Snapmaker Orca** (`C:\Program Files\Snapmaker_Orca\snapmaker-orca.exe`) if the file name ends in `_u1.3mf` (case-insensitive) or **Shift** is held while opening
- **Bambu Studio** (`C:\Program Files\Bambu Studio\bambu-studio.exe`) otherwise

Native C, GUI subsystem (no console flash), exits right after launching the slicer.

## Build (MinGW-w64)

```
windres printer_fork.rc -O coff -o printer_fork.res
gcc -Os -s -municode -mwindows printer_fork.c printer_fork.res -o printer_fork.exe
```

`make_icon.ps1` regenerates `printer_fork.ico` / `printer_fork.png`.
