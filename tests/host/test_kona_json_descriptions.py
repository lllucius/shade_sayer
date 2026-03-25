#!/usr/bin/env python3
"""Validation for natural-language descriptions stored in Kona JSON files."""

import json
import pathlib
import re


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
JSON_FILES = (
    REPO_ROOT / "kona_captures.json",
    REPO_ROOT / "kona_synthetic_tints.json",
)
SENTENCE_END_RE = re.compile(r"[.?!]")
COMPARISON_RE = re.compile(r"\blike\b")
FORBIDDEN_TECHNICAL_TERMS = (
    "rgb",
    "lab",
    "hex",
    "wavelength",
    "accessibility",
    "blind",
    "visually impaired",
)


def _load_swatches(path: pathlib.Path):
    with path.open(encoding="utf-8") as f:
        data = json.load(f)
    return data.get("swatches", [])


def test_descriptions_exist_and_are_single_sentence():
    """Every swatch should have a single-sentence comparison-based description."""
    for path in JSON_FILES:
        swatches = _load_swatches(path)
        assert swatches, f"No swatches found in {path.name}"

        for swatch in swatches:
            description = swatch.get("description")
            swatch_id = swatch.get("id")

            assert isinstance(description, str), f"{path.name} swatch {swatch_id} missing description string"
            assert description.strip() == description, f"{path.name} swatch {swatch_id} description has extra whitespace"
            assert description.endswith("."), f"{path.name} swatch {swatch_id} description must end with a period"
            assert "\n" not in description, f"{path.name} swatch {swatch_id} description must stay on one line"
            assert COMPARISON_RE.search(description), (
                f"{path.name} swatch {swatch_id} description must use a familiar comparison"
            )
            assert len(SENTENCE_END_RE.findall(description)) == 1, (
                f"{path.name} swatch {swatch_id} description must be exactly one sentence"
            )


def test_descriptions_avoid_technical_jargon():
    """Descriptions should avoid technical color terminology and accessibility references."""
    for path in JSON_FILES:
        for swatch in _load_swatches(path):
            description = swatch["description"].lower()
            swatch_id = swatch.get("id")
            for term in FORBIDDEN_TECHNICAL_TERMS:
                assert term not in description, (
                    f"{path.name} swatch {swatch_id} description should not include '{term}'"
                )


if __name__ == "__main__":
    test_descriptions_exist_and_are_single_sentence()
    test_descriptions_avoid_technical_jargon()
    print("kona JSON description tests passed")
