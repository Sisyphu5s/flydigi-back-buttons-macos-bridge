#!/bin/sh
# apple_controller_status.sh - read-only macOS admission diagnostic.
# It separates USB/HID registration from the System Settings UI and from
# optional GameController capabilities such as haptics and battery.
set -eu

driver=$(ioreg -r -c AppleGCHIDUserEventDriver -l -w 0 2>/dev/null || true)
prefs=$(defaults read com.apple.GameController 2>/dev/null || true)

printf '%s\n' "=== Apple GameController admission ==="
if [ -z "$driver" ]; then
    printf '%s\n' "driver: NOT REGISTERED"
    printf '%s\n' "next: unlock macOS, reconnect USB, then rerun"
    exit 2
fi

printf '%s\n' "driver: REGISTERED"
for field in IOClass Transport Manufacturer Product GameControllerCategory \
    GameControllerSupport HIDServiceSupport VendorID ProductID \
    PrimaryUsagePage PrimaryUsage; do
    printf '%s' "$driver" | awk -v key="$field" '
        BEGIN { q = sprintf("%c", 34); needle = q key q " = " }
        index($0, needle) {
            sub("^.*" needle, ""); print key ": " $0; found=1; exit
        }
        END { if (!found) print key ": (not present)" }
    '
done

printf '%s\n' "=== Persisted GameController record ==="
if printf '%s' "$prefs" | grep -q 'name = "Vader 2 Pro"'; then
    printf '%s\n' "record: PRESENT (the controller has been accepted by GameController)"
else
    printf '%s\n' "record: ABSENT"
fi
if printf '%s' "$prefs" | grep -q 'showGCPrefsPane = 1'; then
    printf '%s\n' "prefs pane: ENABLED"
else
    printf '%s\n' "prefs pane: not enabled"
fi
if printf '%s' "$prefs" | grep -q 'supportsHaptics = 1'; then
    printf '%s\n' "native haptics: declared"
else
    printf '%s\n' "native haptics: not declared (use the WebHID bridge)"
fi
if printf '%s' "$prefs" | grep -q 'battery'; then
    printf '%s\n' "native battery: record contains battery data"
else
    printf '%s\n' "native battery: no GameController battery field"
fi

if printf '%s' "$driver" | grep -q "ProductID.*9234" && \
   printf '%s' "$driver" | grep -q "VendorID.*1204"; then
    printf '%s\n' "identity: 04B4:2412 (Vader 2 Pro model match)"
else
    printf '%s\n' "identity: expected 04B4:2412 not found in Apple driver"
fi
printf '%s\n' "note: a System Settings pane can stay stale while this state is already registered; unlock and reopen the pane before changing firmware."
