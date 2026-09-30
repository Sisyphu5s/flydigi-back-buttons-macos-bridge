#!/usr/bin/env python3
"""Check the experimental SCUF descriptor against the installed Apple model."""
from pathlib import Path
import plistlib
import re
import sys

from hid_parse import parse_report_descriptor

sys.path.insert(0, str(Path(__file__).parent / "rp2350" / "test"))
from usb_desc_check import SCUF_DESC

ASSETS = Path("/System/Library/AssetsV2/com_apple_MobileAsset_GameController_DB1")
RELATIVE_MODEL = "Personalities/Corsair/scuff-omega/Default.plist"
EXTRA_NAMES = {
    "button.p1": "BUTTON_M1", "button.p2": "BUTTON_M3",
    "button.p3": "BUTTON_M4", "button.p4": "BUTTON_M2",
    "button.g1": "BUTTON_G1", "button.g2": "BUTTON_G2",
    "button.g3": "BUTTON_G3", "button.g4": "BUTTON_G4",
    "button.g5": "BUTTON_G5",
}


def main():
    for bundle in ASSETS.glob("*.asset/AssetData/GameControllers-Custom.bundle"):
        model_path = bundle / RELATIVE_MODEL
        if model_path.exists():
            break
    else:
        raise SystemExit("Apple SCUF Omega model not installed")

    with (bundle / "Info.plist").open("rb") as file:
        devices = plistlib.load(file)["Devices"]
    matches = [device for device in devices if RELATIVE_MODEL in device.get("Personalities", [])]
    assert len(matches) == 1
    assert matches[0]["IOPropertyMatch"]["VendorID"] == 0x1B1C
    assert matches[0]["IOPropertyMatch"]["ProductID"] == 0x3A28

    with model_path.open("rb") as file:
        model = plistlib.load(file)["Model"]
    elements, _ = parse_report_descriptor(SCUF_DESC)
    inputs = [element for element in elements if element["kind"] == "input" and not element["const"]]
    driver = model["Driver"]
    for item in driver["Elements"]:
        predicate = item["Predicate"]
        usage = re.fullmatch(r"UsagePage == (\d+) AND Usage == (\d+)", predicate)
        hat = re.fullmatch(r"UsageType == 3 AND UsageTypeIndex == 0", predicate)
        assert usage or hat, (item["Identifier"], predicate)
        if usage:
            page, number = map(int, usage.groups())
            hits = [element for element in inputs
                    if (element["usage_page"], element["usage"]) == (page, number)]
        else:
            hits = [element for element in inputs
                    if (element["usage_page"], element["usage"]) == (1, 0x39)]
        assert len(hits) == 1, (item["Identifier"], predicate, len(hits))

    fields = {field["ExtendedIndex"]: field["SourceExpression"]
              for field in driver["Input"]["GamepadEventFields"]}
    physical = {item["Identifier"]: item for item in model["PhysicalInput"]["Elements"]}
    for identifier, name in EXTRA_NAMES.items():
        item = physical[identifier]
        assert item["Type"] == "Button" and item["LocalizedNameKey"] == name
        expression = fields[item["PressedValueSource"]]
        assert expression["InputExpression"]["ElementIdentifier"] == identifier

    buttons = [element for element in inputs if element["usage_page"] == 9]
    assert [element["usage"] for element in buttons] == list(range(1, 30))
    print(f"SCUF model: PASS ({len(driver['Elements'])} predicates, "
          f"{len(EXTRA_NAMES)} named extras, 29 buttons)")


if __name__ == "__main__":
    main()
