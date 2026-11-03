# License

openEMUdriver is licensed under the **GNU Lesser General Public License v2.1 or later**
(LGPL-2.1-or-later).

Protocol knowledge for the Creative/E-MU CA0189 USB audio family was derived from the
officially open-sourced **"EMU CA0189 USB Audio Driver"** (E-MU Systems / Creative
Technology Ltd, 2008), which E-MU released under the GNU Library (LGPL) license:
the macOS driver source published by the E-MU driver project (Wouter1/EMU-driver
carries that code).

Windows-specific code (WinUSB engine, ASIO driver, panel, tools) was written from
scratch for this project.

The ASIO interface plumbing (`include/asiosdk`, `src/asiosdk`) comes from the
Steinberg ASIO SDK and remains under its own license (see `include/asiosdk/LICENSE.txt`).
A minimal `wxdebug.cpp` replacement is provided because the SDK mirrors omit the
DirectShow-baseclasses debug helper that `dllentry.cpp` references.

Third-party components used during development only (not distributed):
Ghidra, pefile/capstone, 7-Zip, WinLibs MinGW-w64, Windows SDK tools (Inf2Cat, signtool).
