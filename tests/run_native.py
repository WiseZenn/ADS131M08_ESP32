"""Build and run the driver against a fake pipelined ADS131M08, without hardware."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--compiler-arg", action="append", default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="ads131m08-test-") as temporary:
        for source in ("test_driver", "test_ble_firmware"):
            executable = Path(temporary) / (
                source + (".exe" if os.name == "nt" else "")
            )
            command = [
                args.compiler,
                *args.compiler_arg,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(root / "tests/support"),
                "-I",
                str(root / "src"),
                str(root / "src/ADS131M08.cpp"),
                str(root / f"tests/{source}.cpp"),
                "-o",
                str(executable),
            ]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
