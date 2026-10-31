# Tolk

64-bit runtime files from Tolk, the screen reader library named by the accessibility template.

- Source: https://github.com/ndarilek/tolk/releases/download/refs/heads/master/tolk.zip
- SHA-256 of that zip: `76879FE7FEFDB553AE30891284903E4B569BBA190E23DBF51E54B5876C254C19`
- `Tolk.dll`: the library. LGPL 3.0 (`LICENSE.txt`).
- `nvdaControllerClient64.dll`: NVDA's controller client (`LICENSE-NVDA.txt`).
- `SAAPI64.dll`: System Access support, loaded by Tolk.

`src/recomp/access.cpp` loads `Tolk.dll` at run time, so the game still starts when it is missing. The build copies the DLLs next to `LuigisMansionRecomp.exe`.
