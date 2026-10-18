#!/bin/bash -e

. ../../include/path.sh

build=_build$ndk_suffix

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf $build
	exit 0
else
	exit 255
fi

# Android provides libvulkan, but the NDK headers (1.3.275) are older
# than the pinned mpv's minimum required Vulkan headers (1.4.309).
# Never fake the pkg-config version: install actual pinned headers for this
# cross-build, preserving Vulkan support and the system's Android loader.
vulkan_headers_version=1.4.309
vulkan_headers_dir="$DIR/deps/vulkan-headers-$vulkan_headers_version"
if [ ! -s "$vulkan_headers_dir/include/vulkan/vulkan_core.h" ]; then
    mkdir -p "$vulkan_headers_dir"
    vulkan_archive=$(mktemp)
    trap 'rm -f "$vulkan_archive"' EXIT
    wget -q "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$vulkan_headers_version.tar.gz" -O "$vulkan_archive"
    tar -xzf "$vulkan_archive" -C "$vulkan_headers_dir" --strip-components=1
    rm -f "$vulkan_archive"
fi
if ! grep -Eq '^#define VK_HEADER_VERSION[[:space:]]+309

unset CC CXX # meson wants these unset

# Fail the build if the fork's PCM tap no longer matches pinned mpv.
# Never silently continue with a libmpv lacking the promised callback.
if [ -f "$DIR/patches/mpv-vad-pcm-tap.patch" ]; then
    if patch -p1 --fuzz=0 --forward --dry-run < "$DIR/patches/mpv-vad-pcm-tap.patch" >/dev/null 2>&1; then
        patch -p1 --fuzz=0 --forward < "$DIR/patches/mpv-vad-pcm-tap.patch"
    elif patch -p1 --fuzz=0 --reverse --dry-run < "$DIR/patches/mpv-vad-pcm-tap.patch" >/dev/null 2>&1; then
        echo "PCM tap already applied."
    else
        echo "ERROR: mpv PCM tap patch does not match pinned source." >&2
        exit 1
    fi
fi

if [ -f "$DIR/patches/mpv-shaders.patch" ]; then
	patch -p1 -N --dry-run < "$DIR/patches/mpv-shaders.patch" >/dev/null 2>&1 && patch -p1 < "$DIR/patches/mpv-shaders.patch" || true
fi

meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	--default-library shared \
	-Diconv=disabled -Dlua=enabled -Dvulkan=enabled \
	-Dlibmpv=true -Dcplayer=false \
	-Dmanpage-build=disabled

ninja -C $build -j$cores
if [ -f $build/libmpv.a ]; then
	echo >&2 "Meson fucked up, forcing rebuild."
	$0 clean
	exec $0 build
fi
DESTDIR="$prefix_dir" ninja -C $build install
 "$vulkan_headers_dir/include/vulkan/vulkan_core.h"; then
    echo "ERROR: pinned Vulkan headers are missing or have an unexpected version." >&2
    exit 1
fi
mkdir -p "$prefix_dir"/include "$prefix_dir"/lib/pkgconfig
cp -R "$vulkan_headers_dir/include/." "$prefix_dir/include/"
cat >"$prefix_dir"/lib/pkgconfig/vulkan.pc <<'END'
Name: Vulkan
Description: Pinned Vulkan-Headers for Android libvulkan
Version: 1.4.309
Libs: -lvulkan
Cflags: -I/usr/local/include
END

unset CC CXX # meson wants these unset

# Fail the build if the fork's PCM tap no longer matches pinned mpv.
# Never silently continue with a libmpv lacking the promised callback.
if [ -f "$DIR/patches/mpv-vad-pcm-tap.patch" ]; then
    if patch -p1 --fuzz=0 --forward --dry-run < "$DIR/patches/mpv-vad-pcm-tap.patch" >/dev/null 2>&1; then
        patch -p1 --fuzz=0 --forward < "$DIR/patches/mpv-vad-pcm-tap.patch"
    elif patch -p1 --fuzz=0 --reverse --dry-run < "$DIR/patches/mpv-vad-pcm-tap.patch" >/dev/null 2>&1; then
        echo "PCM tap already applied."
    else
        echo "ERROR: mpv PCM tap patch does not match pinned source." >&2
        exit 1
    fi
fi

if [ -f "$DIR/patches/mpv-shaders.patch" ]; then
	patch -p1 -N --dry-run < "$DIR/patches/mpv-shaders.patch" >/dev/null 2>&1 && patch -p1 < "$DIR/patches/mpv-shaders.patch" || true
fi

meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	--default-library shared \
	-Diconv=disabled -Dlua=enabled -Dvulkan=enabled \
	-Dlibmpv=true -Dcplayer=false \
	-Dmanpage-build=disabled

ninja -C $build -j$cores
if [ -f $build/libmpv.a ]; then
	echo >&2 "Meson fucked up, forcing rebuild."
	$0 clean
	exec $0 build
fi
DESTDIR="$prefix_dir" ninja -C $build install
