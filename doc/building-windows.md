# Building JDK25 on Windows

Quick guide for building this JDK fork on Windows using Scoop + MSYS2 + Visual Studio.

## Prerequisites

### 1. Install Scoop
```powershell
Set-ExecutionPolicy RemoteSigned -Scope CurrentUser
irm get.scoop.sh | iex
```

### 2. Install build tools via Scoop
```bash
scoop install make
scoop install msys2
scoop bucket add java
scoop install temurin-lts-jdk
```

### 3. Install MSYS2 packages

MSYS2's package manager may have DNS issues in some environments. Download and install manually:

```bash
MSYS2="/c/Users/$USERNAME/scoop/apps/msys2/current"
BASE="https://repo.msys2.org/msys/x86_64"
TMPDIR="/tmp/msys2pkgs"
mkdir -p "$TMPDIR"

# Download packages using curl
for pkg in \
  "m4-1.4.19-2-x86_64.pkg.tar.zst" \
  "diffutils-3.12-1-x86_64.pkg.tar.zst" \
  "make-4.4.1-2-x86_64.pkg.tar.zst" \
  "unzip-6.0-3-x86_64.pkg.tar.zst" \
  "zip-3.0-4-x86_64.pkg.tar.zst" \
  "autoconf2.13-2.13-6-any.pkg.tar.zst" \
  "autoconf2.69-2.69-4-any.pkg.tar.zst" \
  "autoconf2.71-2.71-4-any.pkg.tar.zst" \
  "autoconf2.72-2.72-3-any.pkg.tar.zst" \
  "autoconf-wrapper-20250528-1-any.pkg.tar.zst"; do
  curl -s -L "$BASE/$pkg" -o "$TMPDIR/$pkg"
done

# Install into MSYS2
"$MSYS2/usr/bin/bash.exe" --login -c "
  pacman -U --noconfirm --nodeps /tmp/msys2pkgs/*.pkg.tar.zst
"
```

### 4. Install Visual Studio
Install [Visual Studio 2019 Build Tools](https://visualstudio.microsoft.com/downloads/) (or full VS 2019/2022)
with the **"Desktop development with C++"** workload.

## Configure

Run from Git Bash or any shell:

```bash
MSYS2="/c/Users/$USERNAME/scoop/apps/msys2/current"
BOOT_JDK="/c/Users/$USERNAME/scoop/apps/temurin-lts-jdk/current"

"$MSYS2/usr/bin/bash.exe" --login -c "
  cd '/c/Users/$USERNAME/Documents/GitHub/jdk25'
  bash configure --with-boot-jdk='$BOOT_JDK' --with-toolchain-type=microsoft
"
```

Configuration will be written to `build/windows-x86_64-server-release/`.

## Build

```bash
MSYS2="/c/Users/$USERNAME/scoop/apps/msys2/current"

"$MSYS2/usr/bin/bash.exe" --login -c "
  export COMSPEC='C:\\Windows\\system32\\cmd.exe'
  cd '/c/Users/$USERNAME/Documents/GitHub/jdk25'
  make images
"
```

> **Important:** `COMSPEC` must be set explicitly. Without it, directory junction
> creation (`mklink /J`) fails silently and the build errors out at the `zip-source` step.

## Verify

```bash
./build/windows-x86_64-server-release/images/jdk/bin/java -version
```

Expected output:
```
openjdk version "25.0.2-internal" 2026-01-20
OpenJDK Runtime Environment (build 25.0.2-internal-adhoc.<user>.jdk25)
OpenJDK 64-Bit Server VM (build 25.0.2-internal-adhoc.<user>.jdk25, mixed mode, sharing)
```

## Incremental builds

After changing Java source:
```bash
make java
```

After changing HotSpot C++ source:
```bash
make hotspot
```

Full rebuild:
```bash
make clean && make images
```

## Troubleshooting

| Error | Cause | Fix |
|-------|-------|-----|
| `Cannot find autoconf` | autoconf not in PATH | Install via MSYS2 pacman (see above) |
| `command not found` in ZipSource.gmk | `COMSPEC` is empty | Set `export COMSPEC='C:\Windows\system32\cmd.exe'` |
| `Could not find required tool for UNZIP` | unzip missing from MSYS2 | Install unzip via MSYS2 pacman |
| DNS timeout in pacman | MSYS2 mirror connectivity | Download packages manually with curl and use `pacman -U` |
