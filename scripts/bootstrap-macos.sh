#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    snow_load_qt_policy
    echo 'Usage: bootstrap-macos.sh [macOS-preset] [--skip-dependency-install] [--skip-qt-validation]'
    printf 'Requires Xcode command-line tools, Rust (rustup), Qt %s, CMake >= 4.2, Ninja and pkg-config.\n' "$snow_qt_version"
    exit 0
fi
snow_require_macos
preset=''
if [[ $# -gt 0 && "$1" != --* ]]; then preset="$1"; shift; fi
snow_select_preset "$preset"
skip_dependencies=0
skip_qt=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-dependency-install) skip_dependencies=1 ;;
        --skip-qt-validation) skip_qt=1 ;;
        *) snow_die "Unknown argument: $1" ;;
    esac
    shift
done
xcrun --find clang >/dev/null
if [[ "$skip_qt" == 1 ]]; then
    export PATH="$snow_repo_root/.tools/macos-dev/bin:$snow_repo_root/.tools/macos-media/host/bin:$PATH"
    for tool in cmake ninja cargo rustup pkg-config; do
        command -v "$tool" >/dev/null || snow_die "Missing $tool. Install the prerequisites listed in docs-macos-build.md."
    done
    snow_require_target_tools "$snow_arch"
else
    snow_setup_tools
fi
for tool in git python3; do
    command -v "$tool" >/dev/null || snow_die "Missing $tool. Install the prerequisites listed in docs-macos-build.md."
done
cmake_version="$(cmake --version | head -n 1)"
if [[ ! "$cmake_version" =~ ^cmake[[:space:]]version[[:space:]]([0-9]+)\.([0-9]+) ]] ||
    (( BASH_REMATCH[1] < 4 || (BASH_REMATCH[1] == 4 && BASH_REMATCH[2] < 2) )); then
    snow_die "CMake 4.2 or newer is required; found: ${cmake_version:-unknown}"
fi
cd "$snow_repo_root"
rust_toolchain=1.97.1
rust_toolchain_is_ready() {
    local rust_info rust_host installed_components component
    # `rustup run` checks an existing toolchain without auto-installing it.
    rust_info="$(rustup run "$rust_toolchain" rustc -vV 2>/dev/null)" || return 1
    rust_host="$(sed -n 's/^host: //p' <<< "$rust_info")"
    [[ -n "$rust_host" ]] || return 1
    installed_components="$(rustup component list --toolchain "$rust_toolchain" --installed)" || return 1
    for component in cargo rustc rust-std rustfmt clippy; do
        grep -Fqx "$component-$rust_host" <<< "$installed_components" || return 1
    done
    grep -Fqx "rust-std-$snow_rust_target" <<< "$installed_components"
}
# Builds call bootstrap too. A complete pinned toolchain needs no distribution sync.
if rust_toolchain_is_ready; then
    printf 'Reusing Rust %s with target %s.\n' "$rust_toolchain" "$snow_rust_target"
else
    rustup toolchain install "$rust_toolchain" --profile minimal \
        --component rustfmt --component clippy --target "$snow_rust_target" --no-self-update
fi
# Use the same pinned registry baseline and isolated dynamic/static install roots as Windows.
vcpkg_root="$snow_repo_root/.tools/vcpkg"
vcpkg_baseline=4497409a47f19db373a410a0efb84eca4747adbf
if [[ ! -d "$vcpkg_root/.git" ]]; then
    [[ ! -e "$vcpkg_root" ]] || snow_die "Repository-local vcpkg must be a Git checkout: $vcpkg_root"
    git clone https://github.com/microsoft/vcpkg.git "$vcpkg_root"
fi
vcpkg_head="$(git -C "$vcpkg_root" rev-parse HEAD 2>/dev/null || true)"
if [[ "$vcpkg_head" != "$vcpkg_baseline" ]]; then
    git -C "$vcpkg_root" fetch origin "$vcpkg_baseline" --depth=1
    git -C "$vcpkg_root" checkout --detach "$vcpkg_baseline"
fi
if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
    "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
fi
if [[ "$skip_dependencies" == 0 ]]; then
    "$vcpkg_root/vcpkg" install \
        "--x-manifest-root=$snow_repo_root" \
        "--x-install-root=$snow_vcpkg_installed" \
        "--triplet=$snow_vcpkg_triplet" \
        --x-feature=snow-shot \
        "--overlay-ports=$snow_repo_root/cmake/vcpkg-overlay-ports" \
        "--overlay-triplets=$snow_repo_root/cmake/vcpkg-overlay-triplets" \
        --clean-after-build
fi
