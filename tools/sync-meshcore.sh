#!/bin/sh
# Copies the subset of MeshCore (https://github.com/meshcore-dev/MeshCore, MIT) that WatchApp
# uses into lib/MeshCore. Rerun with a different MESHCORE_REF to update it.
set -eu

MESHCORE_REF=${MESHCORE_REF:-81dbae592ffad40b4212bc0b37e82b38da08f4bc}
APP=$(cd "$(dirname "$0")/.." && pwd)
DEST="$APP/lib/MeshCore"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

git clone -q https://github.com/meshcore-dev/MeshCore.git "$TMP/MeshCore"
git -C "$TMP/MeshCore" checkout -q "$MESHCORE_REF"
SRC="$TMP/MeshCore"

rm -rf "$DEST"
mkdir -p "$DEST/src/helpers/radiolib"
cp "$SRC/license.txt" "$DEST/"
for f in MeshCore.h Dispatcher.h Dispatcher.cpp Identity.h Identity.cpp Mesh.h Mesh.cpp \
         Packet.h Packet.cpp Utils.h Utils.cpp; do
    cp "$SRC/src/$f" "$DEST/src/"
done
for f in BaseChatMesh.h BaseChatMesh.cpp ContactInfo.h ChannelDetails.h AdvertDataHelpers.h \
         AdvertDataHelpers.cpp TxtDataHelpers.h TxtDataHelpers.cpp UTF8Helpers.h \
         StaticPoolPacketManager.h StaticPoolPacketManager.cpp SimpleMeshTables.h \
         IdentityStore.h IdentityStore.cpp ArduinoHelpers.h; do
    cp "$SRC/src/helpers/$f" "$DEST/src/helpers/"
done
for f in RadioLibWrappers.h RadioLibWrappers.cpp CustomSX1262.h CustomSX1262Wrapper.h SX126xReset.h; do
    cp "$SRC/src/helpers/radiolib/$f" "$DEST/src/helpers/radiolib/"
done
# Identity.cpp includes <ed_25519.h>, so the bundled ed25519 sources sit at the include root.
cp "$SRC"/lib/ed25519/*.c "$SRC"/lib/ed25519/*.h "$DEST/src/"
cp "$SRC/lib/ed25519/license.txt" "$DEST/license-ed25519.txt"

cat > "$DEST/library.properties" <<EOF
name=MeshCore
version=0.0.0+$(echo "$MESHCORE_REF" | cut -c1-7)
author=Scott Powell / rippleradios.com
maintainer=WatchApp (vendored subset)
sentence=Subset of MeshCore used by WatchApp, synced from $MESHCORE_REF by tools/sync-meshcore.sh
paragraph=
category=Communication
url=https://github.com/meshcore-dev/MeshCore
architectures=esp32
EOF
echo "Synced MeshCore $MESHCORE_REF into $DEST"
