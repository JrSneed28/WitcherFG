import hashlib
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path


def strict_json(payload):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate JSON key")
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError("invalid JSON constant: " + value)

    return json.loads(payload.decode("utf-8", errors="strict"),
                      object_pairs_hook=unique_object, parse_constant=invalid_constant)


class SupportZipTests(unittest.TestCase):
    def test_cpp_fixture_zip_is_store_only_and_manifest_hashes_match(self):
        archive_path = Path(os.environ.get("WFG_SUPPORT_FIXTURE", str(Path(tempfile.gettempdir()) / "wfg-support-fixture.zip")))
        self.validate_archive(archive_path)

    def test_native_adapter_zip(self):
        if os.name == "nt":
            self.assertIn("WFG_SUPPORT_NATIVE_FIXTURE", os.environ)
            self.validate_archive(Path(os.environ["WFG_SUPPORT_NATIVE_FIXTURE"]))

    def validate_archive(self, archive_path):
        self.assertTrue(archive_path.exists(), "run the C++ support fixture test first")
        with zipfile.ZipFile(archive_path) as archive:
            self.assertTrue(archive.testzip() is None)
            self.assertTrue(all(item.compress_type == zipfile.ZIP_STORED for item in archive.infolist()))
            self.assertTrue(all(".." not in item.filename and "\\" not in item.filename for item in archive.infolist()))
            names = archive.namelist()
            self.assertEqual(len(names), len(set(names)))
            self.assertLessEqual(len(names), 32)
            self.assertLessEqual(sum(item.file_size for item in archive.infolist()), 10 * 1024 * 1024)
            for name in names:
                strict_json(archive.read(name))
                self.assertTrue(name.isascii())
            manifest = strict_json(archive.read("manifest.json"))
            self.assertEqual(set(names), {"manifest.json"} | {item["name"] for item in manifest["entries"]})
            for item in manifest["entries"]:
                self.assertEqual(hashlib.sha256(archive.read(item["name"])).hexdigest(), item["sha256"])
                self.assertEqual(len(archive.read(item["name"])), item["bytes"])
            payload = b"".join(archive.read(item["name"]) for item in manifest["entries"])
            self.assertEqual(hashlib.sha256(payload).hexdigest(), manifest["payload_sha256"])
            self.assertNotIn(b"PRIVATE_SECRET", payload)
            self.assertNotIn(b"PRIVATE_ENV_SECRET", payload)
            if "last-run.json" in names:
                summary = strict_json(archive.read("last-run.json"))
                self.assertEqual(summary["failure"]["code"], "WFG-E-BUILD_UNSUPPORTED")
                self.assertNotIn("action", summary["failure"])
                self.assertEqual(summary["game"]["package"]["value"], "CDPROJEKTSA.Witcher3GOTY")
                self.assertEqual(summary["configuration"]["max_file_mib"], 2)
                self.assertEqual(summary["configuration"]["origins"]["logging.level"], "default")
            if "events.json" in names:
                events = strict_json(archive.read("events.json"))["events"]
                self.assertLessEqual(len(events), 5)
                for event in events:
                    self.assertEqual(event["code"], "WFG-E-BUILD_UNSUPPORTED")
                    self.assertEqual(event["time_utc"], "2026-10-10T12:34:56Z")
                    self.assertEqual(event["fields"][0]["value"], 4)


if __name__ == "__main__":
    unittest.main()
