"""Compile readable native test fixtures with the current production helpers."""
from pathlib import Path
import os
import subprocess
import tempfile


def extract_section(source: Path, start: str, end: str) -> str:
    text = source.read_text()
    try:
        first = text.index(start)
        last = text.index(end, first + len(start))
    except ValueError as error:
        raise ValueError(f"Test extraction markers no longer match {source}") from error
    return text[first:last]


def run_harness(harness: Path, helper: str, *, flags=()) -> None:
    template = harness.read_text()
    marker = "/* SOURCE_UNDER_TEST */"
    if template.count(marker) != 1:
        raise ValueError(f"{harness} must contain exactly one {marker}")
    is_cpp = harness.suffix == ".cpp"
    with tempfile.TemporaryDirectory(prefix=f"{harness.stem}-") as temporary:
        source = Path(temporary) / harness.name
        binary = Path(temporary) / "test"
        source.write_text(template.replace(marker, helper))
        subprocess.run([
            os.environ.get("CXX", "clang++") if is_cpp else os.environ.get("CC", "clang"),
            "-std=c++17" if is_cpp else "-std=c11",
            "-Wall", "-Wextra", "-Werror", *flags,
            str(source), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


def extract_function(source: Path, signature: str) -> str:
    """Extract one known fixture function, without adjacent helper definitions."""
    text = source.read_text()
    first = text.index(signature)
    end = text.index('{', first) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[first:end]
