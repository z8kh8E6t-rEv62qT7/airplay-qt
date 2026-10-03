"""Audit the installed Linux executable and write its minimal runtime README."""

import os
from pathlib import Path
import re
import subprocess
import sys


def output(*command):
    return subprocess.check_output(command, text=True)


def main():
    directory = Path(sys.argv[1])
    if {item.name for item in directory.iterdir()} != {"AirPlayQt"}:
        raise ValueError("Linux payload must contain only the executable before writing README")
    binary = str(directory / "AirPlayQt")
    header = output("readelf", "-h", binary)
    if "Advanced Micro Devices X86-64" not in header:
        raise ValueError("Linux release requires an x86_64 ELF executable")
    dynamic = output("readelf", "-d", binary)
    if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
        raise ValueError("Installed Linux binary must not contain a build or private runtime path")
    sonames = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
    if not sonames or any("/" in name for name in sonames):
        raise ValueError("Missing or absolute ELF dependency names")
    libraries = output("ldd", binary)
    if "not found" in libraries:
        raise ValueError("Unresolved build-time runtime dependency")
    symbols = output("readelf", "--version-info", binary)
    requirements = []
    for prefix in ("GLIBC", "GLIBCXX", "CXXABI"):
        versions = re.findall(r"\b" + prefix + r"_(\d+(?:\.\d+)+)\b", symbols)
        if versions:
            highest = max(versions, key=lambda value: tuple(map(int, value.split("."))))
            requirements.append(f"{prefix}_{highest}")
    qt, openssl, plist, pipewire = output(
        "pkg-config", "--modversion", "Qt6Core", "openssl", "libplist-2.0", "libpipewire-0.3"
    ).splitlines()
    text = f"""# AirPlayQt {os.environ['RELEASE_VERSION']} — Linux x86_64

运行库不随包提供 / Runtime libraries are NOT included.
Built on Ubuntu 24.04 (glibc 2.39); its default repositories do not supply all required library versions.
构建版本 / Built with: Qt {qt} (Core/Gui/Widgets/Network/DBus + system Qt platform plugin), OpenSSL {openssl}, libplist {plist}, PipeWire {pipewire}.
Install ABI-compatible system libraries (Qt 6.8.3+, libplist 2.7+, PipeWire 1.4+); use ldconfig-managed paths, not LD_LIBRARY_PATH.
ELF requirements: {', '.join(requirements)}.
Direct dependencies: {', '.join(sonames)}.

需要运行中的桌面用户会话、PipeWire、WirePlumber 0.5+、Avahi 和 D-Bus；蓝牙输入另需 BlueZ 与对应音频 codec。
Use a running desktop user session with PipeWire, WirePlumber 0.5+, Avahi and D-Bus; Bluetooth capture also needs BlueZ and its audio codecs.

在解压目录执行 / Run in the extracted directory:
```sh
sudo setcap cap_net_bind_service=ep ./AirPlayQt
./AirPlayQt
```
以普通用户运行。替换程序或复制时丢失 capability 后重新 setcap；文件系统须支持 capability。
Run as your normal user. Repeat setcap after replacing/copying the executable if capabilities are lost; the filesystem must support file capabilities.
完整设置 / Setup: https://github.com/{os.environ['GITHUB_REPOSITORY']}/blob/{os.environ['GITHUB_SHA']}/doc/linux/README.md
"""
    (directory / "README.md").write_text(text, encoding="utf-8")
    print(libraries)


if __name__ == "__main__":
    main()
