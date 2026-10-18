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

# Android provides Vulkan, but no pkgconfig file
# you can double-check the version in vulkan_core.h (-> VK_HEADER_VERSION)
mkdir -p "$prefix_dir"/lib/pkgconfig
cat >"$prefix_dir"/lib/pkgconfig/vulkan.pc <<"END"
Name: Vulkan
Description:
Version: 1.3.275
Libs: -lvulkan
Cflags:
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
