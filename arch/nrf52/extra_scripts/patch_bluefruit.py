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


def _patch_ble_advertising(source: Path) -> bool:
    """
    Patch BLEAdvertising.cpp to remove unnecessary stop block.
    Returns True if patch was applied or already applied, False on error.
    """
    try:
        text = source.read_text()
        
        # Check if patch is already applied (stop block doesn't exist)
        stop_block = (
            "  // stop first if current running since we may change advertising data/params\n"
            "  if (_running) {\n"
            "    sd_ble_gap_adv_stop(_hdl);\n"
            "  }\n\n"
        )
        
        if stop_block not in text:
            # Verify patch is applied: check that sd_ble_gap_adv_stop is not called in this context
            if "sd_ble_gap_adv_stop" not in text or "if (_running)" not in text:
                return True  # Already patched
            return False  # Unexpected state
        
        # Apply patch
        text = text.replace(stop_block, "", 1)
        source.write_text(text)
        
        # Verify patch was applied correctly
        verify_text = source.read_text()
        if stop_block in verify_text:
            return False  # Patch failed to apply
        
        return True
    except Exception as e:
        print(f"Bluefruit patch: ERROR patching BLEAdvertising.cpp: {e}")
        return False


def _patch_ble_characteristic(source: Path) -> bool:
    """
    Fix semaphore leak in BLECharacteristic.cpp by commenting out error check.
    
    The bug: conn->releaseHvnPacket() is only called when sd_ble_gatts_hvx fails.
    This causes semaphore leaks when BLE is disabled/disconnecting (returns BLE_ERROR_NOT_ENABLED).
    By commenting out the if statement (lines 743, 744, 746), the semaphore is always released.
    
    Returns True if patch was applied or already applied, False on error.
    """
    try:
        lines = source.read_text().splitlines(keepends=True)
        
        # Line numbers are 1-indexed, array is 0-indexed
        # Lines to comment: 743, 744, 746 (0-indexed: 742, 743, 745)
        lines_to_comment = [742, 743, 745]
        
        # Check if already patched
        already_patched = True
        for line_idx in lines_to_comment:
            if line_idx < len(lines):
                line = lines[line_idx]
                stripped = line.lstrip()
                if stripped and not stripped.startswith('//') and not stripped.startswith('/*'):
                    already_patched = False
                    break
        
        if already_patched:
            return True  # Already patched
        
        # Apply patch
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
        
        if not modified:
            return True  # Nothing to patch
        
        source.write_text(''.join(lines))
        
        # Verify patch was applied correctly
        verify_lines = source.read_text().splitlines(keepends=True)
        for line_idx in lines_to_comment:
            if line_idx < len(verify_lines):
                line = verify_lines[line_idx]
                stripped = line.lstrip()
                if stripped and not stripped.startswith('//') and not stripped.startswith('/*'):
                    return False  # Patch verification failed
        
        return True
    except Exception as e:
        print(f"Bluefruit patch: ERROR patching BLECharacteristic.cpp: {e}")
        return False


def _apply_bluefruit_patch(target, source, env):  # pylint: disable=unused-argument
    framework_path = env.get("PLATFORMFW_DIR")
    if not framework_path:
        framework_path = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")

    if not framework_path:
        print("Bluefruit patch: ERROR - framework directory not found")
        env.Exit(1)
        return

    framework_dir = Path(framework_path)
    patch_failed = False
    
    # Patch BLEAdvertising.cpp
    target_file = framework_dir / "libraries" / "Bluefruit52Lib" / "src" / "BLEAdvertising.cpp"
    if target_file.exists():
        before = target_file.read_text()
        success = _patch_ble_advertising(target_file)
        after = target_file.read_text()
        
        if success:
            if before != after:
                print("Bluefruit patch: OK - Applied BLEAdvertising.cpp updates")
            else:
                print("Bluefruit patch: OK - BLEAdvertising.cpp already up to date")
        else:
            print("Bluefruit patch: FAILED - Failed to patch BLEAdvertising.cpp")
            patch_failed = True
    else:
        print("Bluefruit patch: ERROR - BLEAdvertising.cpp not found")
        patch_failed = True
    
    # Patch BLECharacteristic.cpp
    target_file = framework_dir / "libraries" / "Bluefruit52Lib" / "src" / "BLECharacteristic.cpp"
    if target_file.exists():
        before = target_file.read_text()
        success = _patch_ble_characteristic(target_file)
        after = target_file.read_text()
        
        if success:
            if before != after:
                print("Bluefruit patch: OK - Applied BLECharacteristic.cpp updates (commented lines 743, 744, 746)")
            else:
                print("Bluefruit patch: OK - BLECharacteristic.cpp already up to date")
        else:
            print("Bluefruit patch: FAILED - Failed to patch BLECharacteristic.cpp")
            patch_failed = True
    else:
        print("Bluefruit patch: ERROR - BLECharacteristic.cpp not found")
        patch_failed = True
    
    if patch_failed:
        print("Bluefruit patch: CRITICAL - Patch verification failed! Build aborted.")
        env.Exit(1)


bluefruit_action = env.VerboseAction(_apply_bluefruit_patch, "")
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", bluefruit_action)
_apply_bluefruit_patch(None, None, env)




