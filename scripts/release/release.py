"""Validate a manual release, record build provenance, and publish complete assets.

Only the publish command writes to GitHub. Failed writes are never retried or
rolled back: the tag/draft is evidence for the operator to inspect.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import HTTPRedirectHandler, Request, build_opener


ASSETS = {
    "linux": ("AirPlayQt-linux-NEED_DYLIB.tar.gz",),
    "windows": ("AirPlayQt-windows.7z", "AirPlayQt.vst3-windows.7z"),
    "macos": ("AirPlayQt.app-macos.7z", "AirPlayQt.vst3-macos.7z"),
}
VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"


def release_context():
    if os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch":
        raise ValueError("Releases require workflow_dispatch")
    if os.environ.get("GITHUB_REF") != "refs/heads/master":
        raise ValueError("Releases can only be dispatched from master")
    tag = os.environ.get("RELEASE_TAG", "")
    if not re.fullmatch("v?" + VERSION, tag):
        raise ValueError("Tag must be X.Y.Z or vX.Y.Z (no leading zeroes)")
    return tag, tag.removeprefix("v"), checkout_sha()


def checkout_sha():
    sha = os.environ.get("GITHUB_SHA", "")
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("Missing or invalid trigger SHA")
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if actual != sha:
        raise ValueError(f"Checkout {actual} differs from trigger {sha}")
    return sha


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        return None


class GitHub:
    def __init__(self):
        repository = os.environ["GITHUB_REPOSITORY"]
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ValueError("Invalid repository")
        self.base = f"https://api.github.com/repos/{repository}"
        self.token = os.environ["GH_TOKEN"]
        self.opener = build_opener(NoRedirect())

    def request(self, method, path, payload=None, *, missing_ok=False):
        # Asset upload URLs are returned by GitHub; never send credentials to
        # any other origin, including a redirect.
        url = self.base + path if path.startswith("/") else path
        if not url.startswith(("https://api.github.com/", "https://uploads.github.com/")):
            raise ValueError("Unexpected GitHub API origin")
        binary = isinstance(payload, bytes)
        data = payload if binary else json.dumps(payload).encode() if payload is not None else None
        request = Request(url, data=data, method=method, headers={
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "Content-Type": "application/octet-stream" if binary else "application/json",
            "User-Agent": "AirPlayQt-release",
        })
        try:
            with self.opener.open(request, timeout=180) as response:
                return json.load(response)
        except HTTPError as error:
            error.close()
            if missing_ok and error.code == 404:
                return None
            raise RuntimeError(f"GitHub {method} {url}: HTTP {error.code}; no automatic retry") from error

    def require_unused_tag(self, tag):
        if self.request("GET", f"/git/ref/tags/{tag}", missing_ok=True) is not None:
            raise ValueError(f"Tag already exists: {tag}")
        # Include drafts and paginate, rather than treating the published-only
        # release-by-tag endpoint as proof that no release exists.
        page = 1
        while True:
            releases = self.request("GET", f"/releases?per_page=100&page={page}")
            if any(release["tag_name"] == tag for release in releases):
                raise ValueError(f"Release or draft already exists: {tag}")
            if len(releases) < 100:
                break
            page += 1


def fingerprint(path):
    if not path.is_file() or path.is_symlink() or path.stat().st_size == 0:
        raise ValueError(f"Missing, empty or symlinked asset: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return {"size": path.stat().st_size, "sha256": digest.hexdigest()}


def record(platform, directory, dependencies):
    version = os.environ["RELEASE_VERSION"]
    if not re.fullmatch(VERSION, version):
        raise ValueError("Invalid build version")
    manifest = {
        "platform": platform, "version": version, "sha": checkout_sha(),
        "dependencies": dependencies.read_text(encoding="utf-8"),
        "assets": {name: fingerprint(directory / name) for name in ASSETS[platform]},
    }
    (directory / f"{platform}.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def collect_assets(directory, version, sha):
    assets = {}
    manifests = {}
    for platform, names in ASSETS.items():
        root = directory / f"release-{platform}"
        manifest = json.loads((root / f"{platform}.json").read_text(encoding="utf-8"))
        if (manifest["platform"], manifest["version"], manifest["sha"]) != (platform, version, sha):
            raise ValueError(f"Wrong platform, version or source SHA in {platform} manifest")
        if set(manifest["assets"]) != set(names):
            raise ValueError(f"Incorrect asset list for {platform}")
        if {path.name for path in root.iterdir()} != {*names, f"{platform}.json"}:
            raise ValueError(f"Unexpected or missing files in {root}")
        for name in names:
            path = root / name
            metadata = fingerprint(path)
            if metadata != manifest["assets"][name]:
                raise ValueError(f"Asset checksum/size mismatch: {name}")
            assets[name] = (path, metadata)
        manifests[platform] = manifest
    return assets, manifests


def release_notes(version, sha, assets, manifests):
    lines = [f"AirPlayQt {version}", "", f"Source commit: `{sha}`", "",
             "- Linux x86_64: built on Ubuntu 24.04; runtime libraries are NOT included. See the bundled README.",
             "- Windows x64: application and VST3 include runtime libraries; ASIO driver required for capture.",
             "- macOS arm64: macOS 27.0+, ad-hoc signed.", "",
             "CI builds and packages only; it does not run application tests or VST3 validator.", "",
             "### SHA-256", "", "```text"]
    lines.extend(f"{metadata['sha256']}  {name}" for name, (_, metadata) in assets.items())
    lines.extend(["```", "", "### Build dependencies"])
    for platform, manifest in manifests.items():
        lines.extend(["", f"<details><summary>{platform}</summary>", "", "```text",
                      manifest["dependencies"].strip(), "```", "", "</details>"])
    return "\n".join(lines) + "\n"


def publish(api, tag, version, sha, directory):
    # Complete all local checks before the first remote write.
    assets, manifests = collect_assets(directory, version, sha)
    notes = release_notes(version, sha, assets, manifests)
    api.require_unused_tag(tag)
    api.request("POST", "/git/refs", {"ref": f"refs/tags/{tag}", "sha": sha})
    draft = api.request("POST", "/releases", {
        "tag_name": tag, "target_commitish": sha, "name": tag, "body": notes,
        "draft": True, "prerelease": False,
    })
    print(f"Draft created: {draft['html_url']}", flush=True)
    upload = draft["upload_url"].split("{", 1)[0]
    for name, (path, _) in assets.items():
        api.request("POST", upload + "?" + urlencode({"name": name}), path.read_bytes())
    remote_assets = api.request("GET", f"/releases/{draft['id']}/assets?per_page=100")
    if len(remote_assets) != len(assets) or {item["name"] for item in remote_assets} != set(assets):
        raise ValueError("Draft asset list differs from the five expected assets")
    for item in remote_assets:
        metadata = assets[item["name"]][1]
        if (item["state"] != "uploaded" or item["size"] != metadata["size"]
                or item.get("digest") != "sha256:" + metadata["sha256"]):
            raise ValueError(f"Uploaded asset verification failed: {item['name']}")
    ref = api.request("GET", f"/git/ref/tags/{tag}")
    if ref["object"]["type"] != "commit" or ref["object"]["sha"] != sha:
        raise ValueError("Release tag no longer points to the trigger commit")
    result = api.request("PATCH", f"/releases/{draft['id']}", {"draft": False})
    if result["draft"]:
        raise ValueError("GitHub returned a draft after publication")
    print(f"Published: {result['html_url']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate")
    build = commands.add_parser("record")
    build.add_argument("platform", choices=ASSETS)
    build.add_argument("--directory", type=Path, required=True)
    build.add_argument("--dependencies", type=Path, required=True)
    release = commands.add_parser("publish")
    release.add_argument("--directory", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "record":
        record(args.platform, args.directory, args.dependencies)
        return
    tag, version, sha = release_context()
    api = GitHub()
    if args.command == "validate":
        api.require_unused_tag(tag)
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"version={version}\n")
        print(f"Validated {tag} at {sha}")
    else:
        publish(api, tag, version, sha, args.directory)


if __name__ == "__main__":
    try:
        main()
    except (KeyError, ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Release failed: {error}", file=sys.stderr)
        sys.exit(1)
