#!/usr/bin/env bash
set -euo pipefail

usage() {
	echo "usage: $0 <x86_64-linux-musl|aarch64-linux-musl|aarch64-macos> <version>" >&2
}

if [ "$#" -ne 2 ]; then
	usage
	exit 2
fi

target=$1
version=${2#v}
version_pattern='^[0-9]+[.][0-9]+[.][0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?$'

case "$target" in
x86_64-linux-musl)
	cc="zig cc -target x86_64-linux-musl"
	;;
aarch64-linux-musl)
	cc="zig cc -target aarch64-linux-musl"
	;;
aarch64-macos)
	if [ "$(uname -s)" != "Darwin" ] || [ "$(uname -m)" != "arm64" ]; then
		echo "aarch64-macos packages must be built on an arm64 macOS host" >&2
		exit 1
	fi
	# The Linux targets always cross-compile via `zig cc`. This target builds natively. An inherited
	# `CC` overrides the default host `cc`.
	cc="${CC:-cc}"
	;;
*)
	usage
	exit 2
	;;
esac

if ! [[ "$version" =~ $version_pattern ]]; then
	echo "version must match MAJOR.MINOR.PATCH with an optional prerelease suffix" >&2
	exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# The script uses one build root per target, so several targets packaged in a single checkout do not
# share object files.
build_dir="build/$target"
dist_dir="$repo_root/dist/release/$version"
archive_name="sosig-$version-$target"
stage_dir=$(mktemp -d "${TMPDIR:-/tmp}/sosig-release.XXXXXX")

cleanup() {
	rm -rf "$stage_dir"
}
trap cleanup EXIT

install_dir="$stage_dir/$archive_name"
mkdir -p "$install_dir"

make -C "$repo_root" release CC="$cc" BUILD_DIR="$build_dir" OUT="$install_dir/sosig" VERSION="$version"

cp "$repo_root/README.md" "$install_dir/"
cp "$repo_root/LICENSE.txt" "$install_dir/"
cp "$repo_root/THIRD_PARTY_NOTICES.md" "$install_dir/"

mkdir -p "$dist_dir"
tar -C "$stage_dir" -czf "$dist_dir/$archive_name.tar.gz" "$archive_name"

echo "$dist_dir/$archive_name.tar.gz"
