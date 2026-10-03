"""Check packaged bundle/module metadata without launching application code."""

import json
from pathlib import Path
import plistlib
import re


def check(root, version, platform):
    if platform == "macos":
        for name in ("AirPlayQt.app", "AirPlayQt.vst3"):
            with (root / name / "Contents/Info.plist").open("rb") as source:
                info = plistlib.load(source)
            for key in ("CFBundleVersion", "CFBundleShortVersionString"):
                if info[key] != version:
                    raise ValueError(f"{name}: {key} differs from release version {version}")
            if info["LSMinimumSystemVersion"] != "27.0":
                raise ValueError(f"{name}: wrong minimum macOS version")
    text = (root / "AirPlayQt.vst3/Contents/Resources/moduleinfo.json").read_text(encoding="utf-8")
    # The SDK's JSON5Writer emits quoted keys and trailing commas. Remove
    # only those commas, preserving string contents; do not claim general
    # JSON5 support or alter the signed moduleinfo.json file.
    text = re.sub(r'("(?:\\.|[^"\\])*")|,(?=\s*[}\]])',
                  lambda match: match[1] or "", text)
    module = json.loads(text)
    if module["Version"] != version or not module["Classes"]:
        raise ValueError("Missing classes or incorrect VST3 module version")
    if any(item["Version"] != version for item in module["Classes"]):
        raise ValueError("VST3 factory class version differs from release version")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("macos", "windows"))
    parser.add_argument("root", type=Path)
    parser.add_argument("version")
    args = parser.parse_args()
    check(args.root, args.version, args.platform)
