#!/usr/bin/env python3
"""Validation tests for swatch descriptions in kona_captures.json."""

import json
import pathlib
import re
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
KONA_CAPTURES_PATH = REPO_ROOT / "kona_captures.json"
FORBIDDEN_TERMS = {
    "hex",
    "rgb",
    "lab",
    "wavelength",
    "wavelengths",
    "nanometer",
    "nanometers",
    "nm",
}


class KonaCaptureDescriptionTests(unittest.TestCase):
    """Ensure each swatch has a concise, one-sentence description."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.data = json.loads(KONA_CAPTURES_PATH.read_text(encoding="utf-8"))
        cls.swatches = cls.data["swatches"]

    def test_all_swatches_have_description(self) -> None:
        """Each swatch should include a non-empty description string."""
        self.assertEqual(len(self.swatches), 365)
        for swatch in self.swatches:
            with self.subTest(name=swatch["name"]):
                description = swatch.get("description")
                self.assertIsInstance(description, str)
                self.assertTrue(description.strip())

    def test_descriptions_are_single_sentence(self) -> None:
        """Descriptions should be exactly one sentence ending with a period."""
        for swatch in self.swatches:
            with self.subTest(name=swatch["name"]):
                description = swatch["description"].strip()
                self.assertTrue(description.endswith("."))
                sentence_endings = re.findall(r"[.!?]", description)
                self.assertEqual(sentence_endings, ["."])

    def test_descriptions_avoid_technical_jargon(self) -> None:
        """Descriptions should avoid technical color jargon and code-like values."""
        for swatch in self.swatches:
            with self.subTest(name=swatch["name"]):
                description = swatch["description"].lower()
                for term in FORBIDDEN_TERMS:
                    self.assertNotRegex(description, rf"\b{re.escape(term)}\b")
                self.assertNotRegex(description, r"#[0-9a-f]{3,8}\b")
                self.assertNotRegex(description, r"\b\d{2,4}\s*nm\b")


if __name__ == "__main__":
    unittest.main()
