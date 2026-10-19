#!/usr/bin/env python3
"""Generate the private ScreenScraper build-credential header."""

from __future__ import annotations

import base64
import binascii
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from typing import Optional


DEV_ID_ENV = "XEMU_SCREENSCRAPER_DEV_ID"
DEV_PASSWORD_ENV = "XEMU_SCREENSCRAPER_DEV_PASSWORD"


class SecretError(RuntimeError):
    pass


def read_windows_user_environment(name: str) -> Optional[str]:
    # Base64 keeps all whitespace intact and prevents the credential itself from
    # appearing in the PowerShell command line.
    command = (
        "$value = [Environment]::GetEnvironmentVariable("
        f"'{name}', [EnvironmentVariableTarget]::User); "
        "if ($null -ne $value) { "
        "$bytes = [Text.Encoding]::UTF8.GetBytes($value); "
        "[Console]::Out.Write([Convert]::ToBase64String($bytes)) "
        "}"
    )
    try:
        result = subprocess.run(
            [
                "powershell.exe",
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                command,
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise SecretError(
            f"could not read {name} from the persistent Windows User "
            "environment with powershell.exe"
        ) from exc

    if result.returncode != 0:
        raise SecretError(
            f"powershell.exe could not read {name} from the persistent "
            f"Windows User environment (exit status {result.returncode})"
        )

    encoded = result.stdout.strip()
    if not encoded:
        return None

    try:
        return base64.b64decode(encoded, validate=True).decode("utf-8")
    except (binascii.Error, UnicodeDecodeError) as exc:
        raise SecretError(
            f"powershell.exe returned an invalid encoded value for {name}"
        ) from exc


def read_secret(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        value = read_windows_user_environment(name)
    if not value:
        raise SecretError(
            f"{name} is missing; set it in the current environment or in "
            "the persistent Windows User environment"
        )
    return value


def c_utf8_literal(value: str) -> str:
    # Three-digit octal escapes are unambiguous next to any following byte.
    return '"' + "".join(f"\\{byte:03o}" for byte in value.encode("utf-8")) + '"'


def header_contents(dev_id: str, dev_password: str) -> bytes:
    text = f"""/* Generated at build time. Do not commit this file. */
#ifndef XEMU_SCREENSCRAPER_BUILD_SECRETS_H
#define XEMU_SCREENSCRAPER_BUILD_SECRETS_H

#define XEMU_SCREENSCRAPER_DEV_ID {c_utf8_literal(dev_id)}
#define XEMU_SCREENSCRAPER_DEV_PASSWORD {c_utf8_literal(dev_password)}

#endif /* XEMU_SCREENSCRAPER_BUILD_SECRETS_H */
"""
    return text.encode("ascii")


def restrict_permissions(path: Path) -> None:
    try:
        path.chmod(stat.S_IRUSR | stat.S_IWUSR)
    except OSError:
        # Windows filesystems do not always expose POSIX permission controls.
        pass


def write_if_changed_atomic(path: Path, contents: bytes) -> None:
    try:
        if path.read_bytes() == contents:
            restrict_permissions(path)
            return
    except FileNotFoundError:
        pass

    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent, prefix=f".{path.name}.", suffix=".tmp"
    )
    temporary_path = Path(temporary_name)
    try:
        try:
            os.fchmod(descriptor, stat.S_IRUSR | stat.S_IWUSR)
        except (AttributeError, OSError):
            pass
        with os.fdopen(descriptor, "wb") as temporary_file:
            temporary_file.write(contents)
            temporary_file.flush()
            os.fsync(temporary_file.fileno())
        os.replace(temporary_path, path)
        restrict_permissions(path)
    finally:
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} OUTPUT_HEADER", file=sys.stderr)
        return 2

    try:
        dev_id = read_secret(DEV_ID_ENV)
        dev_password = read_secret(DEV_PASSWORD_ENV)
        write_if_changed_atomic(
            Path(sys.argv[1]), header_contents(dev_id, dev_password)
        )
    except (OSError, SecretError) as exc:
        print(f"ScreenScraper build credential error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
