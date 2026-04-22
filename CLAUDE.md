# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

XiaoZhi is an open-source voice AI assistant built on ESP32, using large language models (Qwen/DeepSeek) for voice interaction and MCP protocol for IoT device control. Current version is 2.2.6 (v1 is on separate branch `v1`, maintained until Feb 2026).

## Build Commands

**Setup ESP-IDF environment (use esp-idf-5.5 for this project):**
```bash
source /home/hrong/workspace/code/esp-idf-5.5/export.sh
```

**List all board variants:**
```bash
python3 scripts/release.py --list-boards
```

**Build a specific board:**
```bash
source /home/hrong/workspace/code/esp-idf-5.5/export.sh
python3 scripts/release.py <board_type> --name <variant_name>
# Example: python3 scripts/release.py esp-vocat
```

**Build all variants of a board:**
```bash
python3 scripts/release.py <board_type>
```

**Build all boards:**
```bash
python3 scripts/release.py all
```

**Package current build (after `idf.py build`):**
```bash
python3 scripts/release.py
```

**Flash to device:**
```bash
source /home/hrong/workspace/code/esp-idf-5.5/export.sh
idf.py -p /dev/ttyACM0 flash
```

**Monitor serial output:**
```bash
source /home/hrong/workspace/code/esp-idf-5.5/export.sh
idf.py -p /dev/ttyACM0 monitor
```

**Format code (required before commit):**
```bash
find main -iname '*.h' -o -iname '*.cc' | xargs clang-format -i
```

**Check formatting:**
```bash
clang-format --dry-run -Werror path/to/file.cpp
```

## WSL2 USB Device Connection (ESP32)

ESP32 devices connected to Windows can be accessed from WSL2 via USBIPD.

**In Windows PowerShell (Administrator):**
```powershell
# List USB devices
usbipd list

# Bind and attach ESP32 device (e.g., busid 3-4)
usbipd bind --busid 3-4
usbipd attach --wsl --busid 3-4
```

**In WSL2:**
```bash
# Verify device is available
ls /dev/ttyACM*

# Flash firmware
source /home/hrong/workspace/code/esp-idf-5.5/export.sh
idf.py -p /dev/ttyACM0 flash

# Monitor serial output
idf.py -p /dev/ttyACM0 monitor
```

**Detach device (when done):**
```powershell
usbipd detach --busid 3-4
```

## Architecture

### Device State Machine

States: `Unknown` → `Starting` → `WifiConfiguring`/`Activating` → `Idle` ↔ `Connecting`/`Listening`/`Speaking` → `Upgrading`/`FatalError`

Key transitions:
- `Starting` → `WifiConfiguring` (WiFi not configured) or `Activating` (WiFi already configured)
- `Activating` → `Idle` (connection established)
- `Idle` ↔ `Connecting`/`Listening`/`Speaking` (voice interaction loop)
- Any state → `WifiConfiguring` (on network error)

### Communication Flow

```
ESP32 Device <--WebSocket/MQTT+UDP--> xiaozhi.me server <--> AI Model (Qwen/DeepSeek)
                            |
                    MCP Protocol for IoT Control
```

Two protocol implementations in `main/protocols/`:
- `websocket_protocol.cc/h` - WebSocket protocol
- `mqtt_protocol.cc/h` - MQTT+UDP hybrid protocol

### Core Components

| Component | Path | Role |
|-----------|------|------|
| `Application` | `main/application.cc/h` | Main singleton orchestrator - handles events, state transitions, network callbacks |
| `DeviceStateMachine` | `main/device_state_machine.cc/h` | Manages device state transitions |
| `Protocol` (base) | `main/protocols/protocol.h` | Abstract base for WebSocket and MQTT+UDP |
| `AudioService` | `main/audio/audio_service.cc/h` | Audio capture/playback, VAD, wake word |
| `McpServer` | `main/mcp_server.cc/h` | Device-side MCP protocol for IoT control |
| `Ota` | `main/ota.cc/h` | Over-the-air firmware updates |
| `Display` | `main/display/` | OLED, LCD, LVGL display drivers |
| `Led` | `main/led/` | LED control (single, GPIO, circular strip) |

### Audio Pipeline

`main/audio/` contains:
- `audio_service.cc/h` - main orchestration
- `codecs/` - hardware codec drivers (ES8311, ES8388, ES8374, ES8389, etc.)
- `wake_word/` - ESP-SR wake word detection
- `processors/` - VAD processing
- `codecs/opus/` - OPUS codec

### Board Configuration

Boards are defined in `main/boards/<manufacturer>/<board>/config.json`:
- `target` - ESP-IDF target (esp32, esp32c3, esp32s3, esp32p4)
- `builds[]` - array of build variants with sdkconfig settings
- Boards must specify `manufacturer` if in a subdirectory

Common board configs live in `main/boards/common/`.

### Partition Tables

V2 partition tables in `partitions/v2/` support an `assets` partition for network-loadable content (wake words, themes, fonts). V1 and V2 partition tables are incompatible for OTA.

## Code Style

Google C++ style with `.clang-format`:
- 4 spaces indentation
- 100 character line width
- Attach-style braces
- Access specifiers indented by -4 spaces
- Pointers/references bind to type (left alignment)

Run `clang-format` before committing. Format the entire project:
```bash
find main -iname '*.h' -o -iname '*.cc' | xargs clang-format -i
```

## Key Files

- `main/application.cc` - main entry point, event loop
- `main/device_state_machine.cc` - state definitions and transitions
- `main/protocols/protocol.h` - protocol abstraction
- `main/mcp_server.cc` - MCP tool registration and handling
- `main/boards/` - all board configurations
- `scripts/release.py` - build automation
- `.github/workflows/build.yml` - CI build workflow
