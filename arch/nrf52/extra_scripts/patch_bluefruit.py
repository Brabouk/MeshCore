"""
Bluefruit BLE Advertising Patch Script

This script removes the unnecessary "stop first if current running" block from
BLEAdvertising.cpp in the Adafruit nRF52 Arduino framework.
SoftDevice v6 API's sd_ble_gap_adv_set_configure() is designed to update advertising data/parameters while advertising is active, so no need for unneccessary stops.

Also patches BLECharacteristic.cpp to fix a semaphore leak bug:
- BLECharacteristic::notify() acquires a semaphore with conn->getHvnPacket()
- The semaphore is only released on error (when sd_ble_gatts_hvx fails)
- This causes semaphore leaks when BLE is disabled/disconnecting, leading to crashes
- Fix: Comment out the error check so conn->releaseHvnPacket() always executes

"""

from pathlib import Path

Import("env")  # pylint: disable=undefined-variable


def _patch_ble_advertising(source: Path) -> None:
    text = source.read_text()

    if "sd_ble_gap_adv_stop" not in text:
        return

    stop_block = (
        "  // stop first if current running since we may change advertising data/params\n"
        "  if (_running) {\n"
        "    sd_ble_gap_adv_stop(_hdl);\n"
        "  }\n\n"
    )

    if stop_block in text:
        text = text.replace(stop_block, "", 1)
        source.write_text(text)


def _patch_ble_characteristic(source: Path) -> None:
    """
    Fix semaphore leak in BLECharacteristic.cpp by commenting out error check.
    
    The bug: conn->releaseHvnPacket() is only called when sd_ble_gatts_hvx fails.
    This causes semaphore leaks when BLE is disabled/disconnecting (returns BLE_ERROR_NOT_ENABLED).
    By commenting out the if statement (lines 743, 744, 746), the semaphore is always released.
    """
    lines = source.read_text().splitlines(keepends=True)
    
    # Line numbers are 1-indexed, array is 0-indexed
    # Lines to comment: 743, 744, 746 (0-indexed: 742, 743, 745)
    lines_to_comment = [742, 743, 745]
    
    modified = False
    for line_idx in lines_to_comment:
        if line_idx < len(lines):
            line = lines[line_idx]
            # Only comment if not already commented
            stripped = line.lstrip()
            if stripped and not stripped.startswith('//') and not stripped.startswith('/*'):
                # Preserve indentation
                indent = len(line) - len(line.lstrip())
                lines[line_idx] = line[:indent] + '//' + line[indent:]
                modified = True
    
    if modified:
        source.write_text(''.join(lines))


def _apply_bluefruit_patch(target, source, env):  # pylint: disable=unused-argument
    framework_path = env.get("PLATFORMFW_DIR")
    if not framework_path:
        framework_path = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")

    if not framework_path:
        print("Bluefruit patch: framework directory not found")
        return

    framework_dir = Path(framework_path)
    
    # Patch BLEAdvertising.cpp
    target = framework_dir / "libraries" / "Bluefruit52Lib" / "src" / "BLEAdvertising.cpp"
    if target.exists():
        before = target.read_text()
        _patch_ble_advertising(target)
        after = target.read_text()
        if before != after:
            print("Bluefruit patch: applied BLEAdvertising.cpp updates")
        else:
            print("Bluefruit patch: BLEAdvertising.cpp already up to date")
    else:
        print("Bluefruit patch: BLEAdvertising.cpp not found")
    
    # Patch BLECharacteristic.cpp
    target = framework_dir / "libraries" / "Bluefruit52Lib" / "src" / "BLECharacteristic.cpp"
    if target.exists():
        before = target.read_text()
        _patch_ble_characteristic(target)
        after = target.read_text()
        if before != after:
            print("Bluefruit patch: applied BLECharacteristic.cpp updates (commented lines 743, 744, 746)")
        else:
            print("Bluefruit patch: BLECharacteristic.cpp already up to date")
    else:
        print("Bluefruit patch: BLECharacteristic.cpp not found")


bluefruit_action = env.VerboseAction(_apply_bluefruit_patch, "")
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", bluefruit_action)
_apply_bluefruit_patch(None, None, env)




