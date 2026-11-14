#!/bin/sh
# Local (~local) packages for trying sherpa-onnx on the graphics card,
# run inside ubuntu:26.04 on the build machine with /b mounted:
#   libonnxruntime-sherpa   onnxruntime 1.29 (built from source, stage in
#                           /b/stage), private in $PRIV
#   onnxruntime-ep-vulkan   the Vulkan plugin EP (onnx-vulkan-rs, built with
#                           cargo, /b/libonnxruntime_ep_vulkan.so), in $PRIV
#   sherpa-onnx             patched ("vulkan" provider), linked to $PRIV
# Not for the PPA yet: onnxruntime and the plugin need source packages.
set -e
umask 022
PRIV=/usr/lib/x86_64-linux-gnu/sherpa-onnx
OUT=/b/out
rm -rf $OUT /b/pkg; mkdir -p $OUT /b/pkg
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
  build-essential debhelper cmake patchelf dpkg-dev fakeroot >/dev/null

# ---------------------------------------------------- libonnxruntime-sherpa
W=/b/pkg/ort; mkdir -p $W/DEBIAN $W$PRIV $W/usr/share/doc/libonnxruntime-sherpa
cp -a /b/stage$PRIV/libonnxruntime.so.1* $W$PRIV/
strip --strip-unneeded $W$PRIV/libonnxruntime.so.1.29.0
cp /b/onnxruntime-1.29.0/LICENSE $W/usr/share/doc/libonnxruntime-sherpa/copyright
echo "libonnxruntime 1 libonnxruntime-sherpa (>= 1.29.0)" > $W/DEBIAN/shlibs
cat > $W/DEBIAN/control <<X
Package: libonnxruntime-sherpa
Version: 1.29.0-1~local
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: libs
Priority: optional
Depends: libc6, libstdc++6, libgcc-s1
Installed-Size: $(du -sk $W/usr | cut -f1)
Description: ONNX Runtime 1.29 for sherpa-onnx (private copy)
 ONNX Runtime built from the 1.29.0 release, CPU only, in $PRIV
 so it does not clash with the distribution's libonnxruntime. sherpa-onnx is
 linked to it; it can load execution provider plugins like the Vulkan one.
X
dpkg-deb --root-owner-group -b $W $OUT/libonnxruntime-sherpa_1.29.0-1~local_amd64.deb

# ---------------------------------------------------- onnxruntime-ep-vulkan
W=/b/pkg/ep; mkdir -p $W/DEBIAN $W$PRIV $W/usr/share/doc/onnxruntime-ep-vulkan
install -m644 /b/libonnxruntime_ep_vulkan.so $W$PRIV/
strip --strip-unneeded $W$PRIV/libonnxruntime_ep_vulkan.so
cat /b/ep-license/LICENSE-MIT > $W/usr/share/doc/onnxruntime-ep-vulkan/copyright
cat > $W/DEBIAN/control <<X
Package: onnxruntime-ep-vulkan
Version: 0.0~git8625ef9-1~local
Architecture: amd64
Maintainer: Grigory Tuboltsev <gtubolcev@gmail.com>
Section: libs
Priority: optional
Depends: libc6, libgcc-s1, libvulkan1, libonnxruntime-sherpa (>= 1.29.0)
Recommends: mesa-vulkan-drivers | vulkan-icd
Installed-Size: $(du -sk $W/usr | cut -f1)
Description: Vulkan execution provider plugin for ONNX Runtime (experimental)
 onnx-vulkan-rs (MIT or Apache-2.0): runs ONNX models on any graphics card
 with a Vulkan driver (AMD, Intel, NVIDIA). sherpa-onnx uses it with the
 provider "vulkan" (sd_sherpa: SherpaGpu 1). One author, started in 2026.
X
dpkg-deb --root-owner-group -b $W $OUT/onnxruntime-ep-vulkan_0.0~git8625ef9-1~local_amd64.deb

# -------------------------------------------------------------- sherpa-onnx
dpkg -i $OUT/libonnxruntime-sherpa_*.deb
mkdir -p /b/ort-include
cp -a /b/stage/usr/include/onnxruntime/. /b/ort-include/
ln -sf $PRIV/libonnxruntime.so.1 $PRIV/libonnxruntime.so
cd /b/sherpa
rm -rf src; cp -a sherpa-onnx-2.0.1 src; cd src
QUILT_PATCHES=debian/patches
for p in $(cat debian/patches/series); do patch -p1 < debian/patches/$p; done
rm -rf debian/patches
sed -i '1s/(\([^)]*\)-1)/(\1-4~local)/' debian/changelog
sed -i 's/libonnxruntime-dev, patchelf/patchelf/; s/(= ${binary:Version}), libonnxruntime-dev,/(= ${binary:Version}),/' debian/control
SHERPA_ONNXRUNTIME_LIB_DIR=$PRIV SHERPA_ONNXRUNTIME_INCLUDE_DIR=/b/ort-include \
  DEB_BUILD_OPTIONS="parallel=5 nocheck noddebs" dpkg-buildpackage -b -uc -us -d
cp ../sherpa-onnx_*~local_amd64.deb ../libsherpa-onnx-dev_*~local_amd64.deb $OUT/
chown -R $HOST_UID:$HOST_UID /b/out /b/pkg /b/sherpa /b/ort-include
