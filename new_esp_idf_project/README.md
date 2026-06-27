# new_esp_idf_project

Minimal ESP-IDF project scaffold.

## Build

Open PowerShell in this folder and run:

```powershell
powershell -ExecutionPolicy Bypass -NoProfile -Command ". .\idf-env.ps1; idf.py build"
```

Or open Command Prompt in this folder and run:

```bat
idf-env.bat
idf.py set-target esp32s3
idf.py build
```

Change `esp32s3` to your board target if needed, for example `esp32`, `esp32c3`, or `esp32s3`.

## Flash and monitor

```powershell
idf.py -p COMx flash monitor
```

Replace `COMx` with your serial port.
