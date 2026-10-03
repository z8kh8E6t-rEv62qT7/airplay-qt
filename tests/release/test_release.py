"""Local release-script checks. No builds, network access, or GitHub writes."""

import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess
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
import linux_readme

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

    def test_release_notes_describe_arch_runtime_requirements(self):
        assets, manifests = release.collect_assets(self.root, "1.2.3", SHA)
        notes = release.release_notes("1.2.3", SHA, assets, manifests)
        self.assertIn("built in Arch Linux with precompiled system packages", notes)
        self.assertIn("runtime libraries are NOT included", notes)
        self.assertNotIn("Ubuntu 24.04", notes)

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


class LinuxReadmeChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        binary = self.root / "AirPlayQt"
        binary.write_bytes(b"test executable")
        self.responses = {
            ("readelf", "-h", str(binary)): "Machine: Advanced Micro Devices X86-64\n",
            ("readelf", "-d", str(binary)): "(NEEDED) Shared library: [libQt6Core.so.6]\n",
            ("ldd", str(binary)): "libQt6Core.so.6 => /usr/lib/libQt6Core.so.6\n",
            ("readelf", "--version-info", str(binary)):
                "GLIBC_2.9 GLIBC_2.39 GLIBCXX_3.4.9 GLIBCXX_3.4.33 CXXABI_1.3.15\n",
            ("pkg-config", "--modversion", "Qt6Core", "openssl", "libplist-2.0", "libpipewire-0.3"):
                "6.11.2\n3.6.0\n2.7.0\n1.6.9\n",
        }
        for patcher in (
            patch.dict(os.environ, ENV, clear=True),
            patch.object(sys, "argv", ["linux_readme.py", str(self.root)]),
            patch.object(linux_readme, "output", side_effect=lambda *args: self.responses[args]),
            patch.object(linux_readme.platform, "freedesktop_os_release",
                         return_value={"PRETTY_NAME": "Arch Linux"}),
            patch("builtins.print"),
        ):
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_readme_uses_actual_distribution_and_dependency_versions(self):
        linux_readme.main()
        text = (self.root / "README.md").read_text(encoding="utf-8")
        for expected in ("Built on: Arch Linux", "Qt 6.11.2", "OpenSSL 3.6.0",
                         "libplist 2.7.0", "PipeWire 1.6.9",
                         "ELF requirements: GLIBC_2.39, GLIBCXX_3.4.33, CXXABI_1.3.15",
                         "Compatibility with older distributions is not guaranteed",
                         f"/blob/{SHA}/doc/linux/README.md"):
            self.assertIn(expected, text)
        self.assertNotIn("Ubuntu 24.04", text)
        self.assertNotIn("Qt 6.8.3+", text)
        self.assertEqual({item.name for item in self.root.iterdir()}, {"AirPlayQt", "README.md"})

    def test_invalid_elf_or_missing_library_prevents_readme_creation(self):
        binary = str(self.root / "AirPlayQt")
        cases = (
            (("readelf", "-h", binary), "Machine: AArch64", "x86_64"),
            (("readelf", "-d", binary), "(RPATH) Library rpath: [/workspace/build]", "runtime path"),
            (("readelf", "-d", binary), "(RUNPATH) Library runpath: [/usr/local/lib]", "runtime path"),
            (("readelf", "-d", binary), "", "dependency names"),
            (("readelf", "-d", binary), "(NEEDED) Shared library: [/tmp/libQt6Core.so.6]", "dependency names"),
            (("ldd", binary), "libQt6Core.so.6 => not found", "Unresolved"),
        )
        for command, response, error in cases:
            with self.subTest(command=command, response=response):
                with patch.dict(self.responses, {command: response}):
                    with self.assertRaisesRegex(ValueError, error):
                        linux_readme.main()
                self.assertFalse((self.root / "README.md").exists())

    def test_missing_system_information_fails_instead_of_inventing_build_environment(self):
        with patch.object(linux_readme.platform, "freedesktop_os_release", side_effect=OSError):
            with self.assertRaises(OSError):
                linux_readme.main()
        self.assertFalse((self.root / "README.md").exists())


class BuildScriptChecks(unittest.TestCase):
    def test_windows_multimedia_headers_keep_windows_first(self):
        text = (SCRIPTS.parents[1] / "src/vst3/NativeWindows.cpp").read_text(encoding="utf-8")
        self.assertLess(text.index("#include <windows.h>"), text.index("#include <mmsystem.h>"))
        self.assertIn("clang-format off", text)
        self.assertIn("clang-format on", text)

    def test_windows_msys_root_conversion_includes_subdirectory(self):
        text = (SCRIPTS / "windows.sh").read_text(encoding="utf-8")
        start = text.index('plist=')
        end = text.index("printf 'Dependency paths:", start)
        # Model the runner's mount mapping, including '/' for its install root.
        mapping = '''
cygpath() {
  [[ "$1" == -u ]]
  case "$2" in
    "$MSYS_ROOT") printf '/' ;;
    "$MSYS_ROOT/clang64") printf '/clang64' ;;
    *) return 91 ;;
  esac
}
'''
        for root in ("D:/a/_temp/msys64", "C:/path with spaces/msys64"):
            with self.subTest(root=root):
                result = self.run_script(f'MSYS_ROOT="{root}"\n' + mapping + text[start:end]
                                         + 'printf "%s" "$asio/common/iasiodrv.h"\n')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "/clang64/include/asiosdk/common/iasiodrv.h")

    def test_macos_version_audit_rejects_either_incorrect_bundle_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            info = {"CFBundleVersion": "1.0.1", "CFBundleShortVersionString": "1.0.1",
                    "LSMinimumSystemVersion": "27.0"}
            plists = []
            for name in ("AirPlayQt.app", "AirPlayQt.vst3"):
                path = root / name / "Contents/Info.plist"
                path.parent.mkdir(parents=True)
                with path.open("wb") as file:
                    plistlib.dump(info, file)
                plists.append(path)
            resources = root / "AirPlayQt.vst3/Contents/Resources"
            resources.mkdir()
            (resources / "moduleinfo.json").write_text(
                '{"Version":"1.0.1","Classes":[{"Version":"1.0.1"}]}', encoding="utf-8")
            check_version.check(root, "1.0.1", "macos")
            for path in plists:
                for key in ("CFBundleVersion", "CFBundleShortVersionString"):
                    with self.subTest(bundle=path.parent.parent.name, key=key):
                        with path.open("wb") as file:
                            plistlib.dump(dict(info, **{key: "0.1.0"}), file)
                        with self.assertRaisesRegex(ValueError, key):
                            check_version.check(root, "1.0.1", "macos")
                        with path.open("wb") as file:
                            plistlib.dump(info, file)

    def run_script(self, body):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "fixture.sh"
            script.write_text('set -euo pipefail\nsource "$1"\n' + body, encoding="utf-8")
            return subprocess.run(
                ["bash", str(script), str(SCRIPTS / "logging.sh")],
                capture_output=True, text=True, check=False,
            )

    def test_logging_reports_success_without_changing_stdout(self):
        result = self.run_script('release_phase configure\nprintf "payload\\n"\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "payload\n")
        self.assertIn("starting=configure", result.stderr)
        self.assertRegex(result.stderr, r"fixture.sh:4")
        self.assertRegex(result.stderr, r"total_elapsed=\d+s status=0")

    def test_logging_preserves_failures_and_reports_location(self):
        for body, status in (
            ('bash -c "exit 23"', 23),
            ('value=$(bash -c "exit 24")', 24),
            ('(bash -c "exit 25")', 25),
            ('bash -c "exit 26" | cat', 26),
            # Like a failure inside Conda activation with xtrace disabled.
            ('set +x\nactivate_fixture() { test -f /missing-release-fixture; }\nactivate_fixture', 1),
        ):
            with self.subTest(body=body):
                result = self.run_script('release_phase dependencies\n' + body + '\nprintf "unreachable"\n')
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertNotIn("unreachable", result.stdout)
                self.assertIn(f"ERROR phase=dependencies status={status}", result.stderr)
                self.assertRegex(result.stderr, r"at=.*fixture.sh:\d+ command=")
                self.assertRegex(result.stderr, rf"finished .*status={status}")

    def test_logging_reports_explicit_exit_and_unbound_variable(self):
        for body, status in (('exit 27', 27), ('printf "%s" "$RELEASE_UNSET_FIXTURE"', 1)):
            with self.subTest(body=body):
                result = self.run_script('unset RELEASE_UNSET_FIXTURE\n' + body)
                self.assertEqual(result.returncode, status)
                self.assertRegex(result.stderr, rf"finished .*status={status}")

    def test_macos_plugin_copy_materializes_symlink_and_rejects_missing_target(self):
        template = (SCRIPTS.parents[1] / "cmake/PackageMacRun.cmake.in").read_text(encoding="utf-8")
        start = template.index('file(MAKE_DIRECTORY "${app}/Contents/PlugIns/platforms")')
        end = template.index('mac_run("Qt official deployment"', start)
        # Exercise the actual staging commands without compiling or deploying Qt.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            real = root / "qt-real.dylib"
            real.write_bytes(b"fixture plugin payload")
            link = root / "libqcocoa.dylib"
            link.symlink_to(real.name)
            script = root / "copy.cmake"
            script.write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                f'set(app "{root.as_posix()}/AirPlayQt.app")\n'
                f'set(cocoa "{link.as_posix()}")\n' + template[start:end], encoding="utf-8",
            )
            result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            target = root / "AirPlayQt.app/Contents/PlugIns/platforms/libqcocoa.dylib"
            real.unlink()
            self.assertFalse(target.is_symlink())
            self.assertEqual(target.read_bytes(), b"fixture plugin payload")
            result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
