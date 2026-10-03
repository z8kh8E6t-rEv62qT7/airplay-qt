"""Local release-script checks. No builds, network access, or GitHub writes."""

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlsplit


SCRIPTS = Path(__file__).resolve().parents[2] / "scripts/release"
sys.path.insert(0, str(SCRIPTS))
import release
import check_version

SHA = "a" * 40
ENV = {
    "GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REF": "refs/heads/master",
    "GITHUB_SHA": SHA, "RELEASE_TAG": "v1.2.3", "RELEASE_VERSION": "1.2.3",
    "GITHUB_REPOSITORY": "owner/repo", "GH_TOKEN": "test-token",
}


class ReleaseChecks(unittest.TestCase):
    def setUp(self):
        self.env = patch.dict(os.environ, ENV, clear=True)
        self.env.start()
        self.addCleanup(self.env.stop)
        self.checkout = patch.object(release.subprocess, "check_output", return_value=SHA + "\n")
        self.checkout.start()
        self.addCleanup(self.checkout.stop)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for platform, names in release.ASSETS.items():
            directory = self.root / f"release-{platform}"
            directory.mkdir()
            for name in names:
                (directory / name).write_bytes(name.encode())
            dependencies = self.root / "dependencies.txt"
            dependencies.write_text("fixed dependency versions\n")
            release.record(platform, directory, dependencies)
        self.api = Mock()
        self.uploaded = []

        def request(method, path, payload=None):
            if method == "POST" and path == "/git/refs":
                self.assertEqual(payload, {"ref": "refs/tags/v1.2.3", "sha": SHA})
                return {}
            if method == "POST" and path == "/releases":
                self.assertTrue(payload["draft"])
                self.assertEqual(payload["target_commitish"], SHA)
                return {"id": 42, "html_url": "https://github.com/owner/repo/releases/42",
                        "upload_url": "https://uploads.github.com/repos/owner/repo/releases/42/assets{?name,label}"}
            if path.startswith("https://uploads.github.com/"):
                self.uploaded.append({"name": parse_qs(urlsplit(path).query)["name"][0],
                                      "size": len(payload), "state": "uploaded",
                                      "digest": "sha256:" + hashlib.sha256(payload).hexdigest()})
                return self.uploaded[-1]
            if path == "/releases/42/assets?per_page=100":
                return self.uploaded
            if path == "/git/ref/tags/v1.2.3":
                return {"object": {"type": "commit", "sha": SHA}}
            if method == "PATCH":
                self.assertEqual(len(self.uploaded), 5)
                return {"draft": False, "html_url": "https://github.com/owner/repo/releases/tag/v1.2.3"}
            raise AssertionError((method, path, payload))

        self.request = request
        self.api.request.side_effect = request

    def publish(self):
        release.publish(self.api, "v1.2.3", "1.2.3", SHA, self.root)

    def assert_not_published(self):
        self.assertFalse(any(call.args[0] == "PATCH" for call in self.api.request.call_args_list))

    def test_context_accepts_only_manual_master_and_numeric_versions(self):
        for tag in ("1.2.3", "v1.2.3", "0.1.0"):
            with self.subTest(tag=tag), patch.dict(os.environ, RELEASE_TAG=tag):
                self.assertEqual(release.release_context(), (tag, tag.removeprefix("v"), SHA))
        for key, value in (("GITHUB_REF", "refs/heads/feature"), ("GITHUB_REF", "refs/tags/v1.2.3"),
                           ("GITHUB_EVENT_NAME", "push"), ("RELEASE_TAG", "v01.2.3"),
                           ("RELEASE_TAG", "v1.2.3\n"), ("RELEASE_TAG", "1.2.3-rc1"),
                           ("RELEASE_TAG", "$(touch bad)"), ("GITHUB_SHA", "master")):
            with self.subTest(key=key, value=value), patch.dict(os.environ, {key: value}):
                with self.assertRaises(ValueError):
                    release.release_context()

    def test_checkout_must_be_trigger_commit(self):
        with patch.object(release.subprocess, "check_output", return_value="b" * 40):
            with self.assertRaisesRegex(ValueError, "differs from trigger"):
                release.release_context()

    def test_existing_tag_and_paginated_draft_are_rejected(self):
        api = release.GitHub()
        with patch.object(api, "request", return_value={"object": {}}):
            with self.assertRaisesRegex(ValueError, "Tag already exists"):
                api.require_unused_tag("v1.2.3")
        pages = [None, [{"tag_name": "old"}] * 100, [{"tag_name": "v1.2.3", "draft": True}]]
        with patch.object(api, "request", side_effect=pages) as request:
            with self.assertRaisesRegex(ValueError, "draft already exists"):
                api.require_unused_tag("v1.2.3")
            self.assertEqual(request.call_args.args[1], "/releases?per_page=100&page=2")

    def test_only_404_means_absent_and_redirects_are_not_followed(self):
        api = release.GitHub()
        for status in (403, 429, 500, 302):
            with self.subTest(status=status), patch.object(api.opener, "open", side_effect=HTTPError(
                    "https://api.github.com", status, "failure", {}, None)):
                with self.assertRaises(RuntimeError):
                    api.request("GET", "/git/ref/tags/v1.2.3", missing_ok=True)
        with patch.object(api.opener, "open", side_effect=HTTPError(
                "https://api.github.com", 404, "missing", {}, None)):
            self.assertIsNone(api.request("GET", "/git/ref/tags/v1.2.3", missing_ok=True))
        self.assertIsNone(release.NoRedirect().redirect_request(None, None, 302, "", {}, "https://example.com"))
        with self.assertRaises(ValueError):
            api.request("POST", "https://example.com/upload", b"asset")

    def test_complete_release_is_published_at_exact_sha(self):
        self.publish()
        self.api.require_unused_tag.assert_called_once_with("v1.2.3")
        self.assertEqual(self.api.request.call_args.args, ("PATCH", "/releases/42", {"draft": False}))
        self.assertFalse(any("heads/master" in call.args[1] for call in self.api.request.call_args_list))

    def test_corrupt_missing_or_unexpected_assets_prevent_all_writes(self):
        directory = self.root / "release-linux"
        path = directory / release.ASSETS["linux"][0]
        original = path.read_bytes()
        for problem in ("corrupt", "missing", "extra", "empty"):
            with self.subTest(problem=problem):
                path.write_bytes(original)
                extra = directory / "unexpected.txt"
                if problem == "corrupt":
                    path.write_bytes(b"corruption")
                elif problem == "missing":
                    path.unlink()
                elif problem == "extra":
                    extra.write_text("unexpected")
                else:
                    path.write_bytes(b"")
                with self.assertRaises(ValueError):
                    self.publish()
                self.api.request.assert_not_called()
                if extra.exists():
                    extra.unlink()

    def test_wrong_manifest_sha_or_version_prevents_all_writes(self):
        path = self.root / "release-linux/linux.json"
        original = json.loads(path.read_text())
        for key, value in (("sha", "b" * 40), ("version", "9.9.9"), ("platform", "windows")):
            with self.subTest(key=key):
                path.write_text(json.dumps({**original, key: value}))
                with self.assertRaises(ValueError):
                    self.publish()
                self.api.request.assert_not_called()

    def test_duplicate_found_at_publication_prevents_all_writes(self):
        self.api.require_unused_tag.side_effect = ValueError("Tag already exists")
        with self.assertRaises(ValueError):
            self.publish()
        self.api.request.assert_not_called()

    def test_tag_creation_draft_creation_and_upload_failure_never_publish(self):
        for stage in ("tag", "draft", "third upload"):
            with self.subTest(stage=stage):
                self.api.reset_mock()
                self.uploaded.clear()

                def fail(method, path, payload=None):
                    if ((stage == "tag" and path == "/git/refs")
                            or (stage == "draft" and path == "/releases")
                            or (stage == "third upload" and path.startswith("https://uploads.")
                                and len(self.uploaded) == 2)):
                        raise RuntimeError("simulated failure")
                    return self.request(method, path, payload)

                self.api.request.side_effect = fail
                with self.assertRaises(RuntimeError):
                    self.publish()
                self.assert_not_published()
                self.assertFalse(any(call.args[0] == "DELETE" for call in self.api.request.call_args_list))

    def test_remote_missing_digest_bad_size_or_moved_tag_blocks_publication(self):
        for fault in ("missing digest", "size", "tag", "asset list"):
            with self.subTest(fault=fault):
                self.api.reset_mock()
                self.uploaded.clear()

                def alter(method, path, payload=None):
                    result = self.request(method, path, payload)
                    if path == "/releases/42/assets?per_page=100":
                        if fault == "missing digest":
                            result[0].pop("digest")
                        elif fault == "size":
                            result[0]["size"] += 1
                        elif fault == "asset list":
                            result.pop()
                    if fault == "tag" and path == "/git/ref/tags/v1.2.3":
                        result["object"]["sha"] = "b" * 40
                    return result

                self.api.request.side_effect = alter
                with self.assertRaises(ValueError):
                    self.publish()
                self.assert_not_published()

    def test_sdk_trailing_commas_are_supported_and_factory_versions_checked(self):
        resources = self.root / "AirPlayQt.vst3/Contents/Resources"
        resources.mkdir(parents=True)
        metadata = resources / "moduleinfo.json"
        text = '{"Version":"1.2.3", "Classes":[{"Version":"1.2.3", "Name":"literal ,}",},],}'
        metadata.write_text(text)
        check_version.check(self.root, "1.2.3", "windows")
        self.assertEqual(metadata.read_text(), text)
        metadata.write_text(text.replace('"Version":"1.2.3", "Name"', '"Version":"0.1.0", "Name"'))
        with self.assertRaisesRegex(ValueError, "factory class version"):
            check_version.check(self.root, "1.2.3", "windows")


if __name__ == "__main__":
    unittest.main()
