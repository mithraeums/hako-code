#!/usr/bin/env sh
# hako-code installer. Fetches the latest GitHub release for this OS/arch, or
# builds from source where there is no release asset (iSh: 32-bit x86, musl).
# Usage:  curl -fsSL https://mithraeums.github.io/hako.sh | sh
# Env:    REPO=owner/hako-code   (override default)
#         PREFIX=/usr/local       (install dir; defaults to ~/.local if not writable)
#         VERIFY=0                (skip sha256 verify; default 1)
#         SOURCE=1                (force a source build even where a release exists)

set -eu

REPO="${REPO:-mithraeums/hako-code}"
PREFIX="${PREFIX:-}"
VERIFY="${VERIFY:-1}"
SOURCE="${SOURCE:-0}"

uname_s="$(uname -s)"
uname_m="$(uname -m)"

case "$uname_s" in
	Linux*)
		case "$uname_m" in
			x86_64|amd64)   asset="hako-code-linux-x86_64.tar.gz";   dirname="hako-code-linux-x86_64";   bin=hako; ext=tar.gz ;;
			arm64|aarch64)  asset="hako-code-linux-arm64.tar.gz";    dirname="hako-code-linux-arm64";    bin=hako; ext=tar.gz ;;
			*) asset=""; bin=hako ;;     # iSh (i686) and the rest: source
		esac
		;;
	Darwin*)
		asset="hako-code-macos-universal.tar.gz"; dirname="hako-code-macos-universal"; bin=hako; ext=tar.gz
		;;
	FreeBSD*)
		case "$uname_m" in
			amd64|x86_64) asset="hako-code-freebsd-x86_64.tar.gz"; dirname="hako-code-freebsd-x86_64"; bin=hako; ext=tar.gz ;;
			*) asset=""; bin=hako ;;
		esac
		;;
	MINGW*|MSYS*|CYGWIN*)
		asset="hako-code-windows-x86_64.zip"; dirname="hako-code-windows-x86_64"; bin=hako.exe; ext=zip
		;;
	*) asset=""; bin=hako ;;
esac

if [ -z "$PREFIX" ]; then
	if [ -w "/usr/local/bin" ] || [ "$(id -u)" = "0" ]; then
		PREFIX=/usr/local
	else
		PREFIX="${HOME}/.local"
	fi
fi
mkdir -p "${PREFIX}/bin"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

finish() {
	echo "installed: ${PREFIX}/bin/${bin}"
	"${PREFIX}/bin/${bin}" --version || true
	case ":${PATH}:" in
		*":${PREFIX}/bin:"*) ;;
		*) echo "note: ${PREFIX}/bin not in PATH. add to your shell rc:"
		   echo "    export PATH=\"${PREFIX}/bin:\$PATH\"" ;;
	esac
	exit 0
}

# One C file and pthread: a source build needs a compiler and nothing else.
build_from_source() {
	command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || {
		echo "no C compiler. on iSh: apk add build-base git curl" >&2; exit 1; }
	command -v make >/dev/null 2>&1 || { echo "make not found. on iSh: apk add build-base" >&2; exit 1; }
	command -v git  >/dev/null 2>&1 || { echo "git not found. on iSh: apk add git" >&2; exit 1; }
	echo "building hako from source (${uname_s} ${uname_m})..."
	git clone --depth 1 "https://github.com/${REPO}.git" "${tmp}/src" >/dev/null 2>&1
	(cd "${tmp}/src" && make WEB=1) || { echo "build failed" >&2; exit 1; }
	install -m 0755 "${tmp}/src/hako" "${PREFIX}/bin/hako"
	finish
}

if [ "$SOURCE" = 1 ] || [ -z "$asset" ]; then
	[ -z "$asset" ] && echo "no release asset for ${uname_s} ${uname_m}; building from source."
	build_from_source
fi

base="${REPO}"
api="https://api.github.com/repos/${base}/releases/latest"
echo "fetching latest release of ${base}..."

# parse tag_name
tag="$(curl -fsSL "$api" | grep -m1 '"tag_name":' | sed -E 's/.*"tag_name": *"([^"]+)".*/\1/' || true)"
if [ -z "$tag" ]; then
	echo "could not resolve latest tag (rate-limited?); falling back to a source build" >&2
	build_from_source
fi
echo "latest: $tag"

url="https://github.com/${base}/releases/download/${tag}/${asset}"
sha_name="${asset%.tar.gz}"
sha_name="${sha_name%.zip}.sha256"
sha_url="https://github.com/${base}/releases/download/${tag}/${sha_name}"

echo "downloading $asset"
curl -fL --progress-bar -o "${tmp}/${asset}" "$url"

case "$ext" in
	tar.gz) tar -xzf "${tmp}/${asset}" -C "$tmp" ;;
	zip)    unzip -q "${tmp}/${asset}" -d "$tmp" ;;
esac

if [ "$VERIFY" = "1" ]; then
	echo "verifying sha256..."
	if curl -fsSL -o "${tmp}/${sha_name}" "$sha_url"; then
		(cd "${tmp}/${dirname}" && \
			if command -v sha256sum >/dev/null 2>&1; then sha256sum -c "../${sha_name}" >/dev/null; \
			elif command -v shasum    >/dev/null 2>&1; then shasum -a 256 -c "../${sha_name}" >/dev/null; \
			elif command -v sha256    >/dev/null 2>&1; then \
				want="$(awk '{print $1}' "../${sha_name}")"; \
				got="$(sha256 -q "$bin")"; \
				[ "$want" = "$got" ] || { echo "sha256 mismatch" >&2; exit 1; }; \
			else echo "no sha256 tool found, skipping verify" >&2; \
			fi) || { echo "sha256 verify FAILED" >&2; exit 1; }
		echo "sha256 ok."
	else
		echo "warning: sha sidecar missing — skipping verify (set VERIFY=0 to silence)" >&2
	fi
fi

install -m 0755 "${tmp}/${dirname}/${bin}" "${PREFIX}/bin/${bin}"

# macOS Gatekeeper: strip the quarantine xattr so first run isn't blocked.
if [ "$uname_s" = "Darwin" ] && command -v xattr >/dev/null 2>&1; then
	xattr -d com.apple.quarantine "${PREFIX}/bin/${bin}" 2>/dev/null || true
fi

finish
