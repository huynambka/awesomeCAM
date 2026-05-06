#!/system/bin/sh
MODDIR="${0%/*}"
CAMDIR=/data/camera
LOGTAG='sepolicy-patcher'

logi() {
  log -t "$LOGTAG" -p i "$*" 2>/dev/null || echo "I:$LOGTAG: $*"
}

mkdir -p "$CAMDIR"
chmod 0755 "$CAMDIR" 2>/dev/null || true
chown root:root "$CAMDIR" 2>/dev/null || true

# Directory must be searchable by cameraserver/mediaextractor/app rules.
chcon u:object_r:system_data_file:s0 "$CAMDIR" 2>/dev/null || true

# Default files: stable data label.
for file in "$CAMDIR"/*; do
  [ -e "$file" ] || continue
  [ -d "$file" ] && continue
  chmod 0644 "$file" 2>/dev/null || true
  chown root:root "$file" 2>/dev/null || true
  chcon u:object_r:system_data_file:s0 "$file" 2>/dev/null || true
done

# MP4 source: dedicated readable media source label.
if [ -f "$CAMDIR/input.mp4" ]; then
  chmod 0644 "$CAMDIR/input.mp4" 2>/dev/null || true
  chown root:root "$CAMDIR/input.mp4" 2>/dev/null || true
  chcon u:object_r:awesomecam_source_file:s0 "$CAMDIR/input.mp4" 2>/dev/null || true
  logi "relabeled $CAMDIR/input.mp4 -> awesomecam_source_file"
fi

# Offset scanner cache. cameraserver writes this after signature scan.
touch "$CAMDIR/awesomecam_offsets.conf" 2>/dev/null || true
chmod 0666 "$CAMDIR/awesomecam_offsets.conf" 2>/dev/null || true
chown root:root "$CAMDIR/awesomecam_offsets.conf" 2>/dev/null || true
chcon u:object_r:awesomecam_config_file:s0 "$CAMDIR/awesomecam_offsets.conf" 2>/dev/null || true
logi "prepared $CAMDIR/awesomecam_offsets.conf -> awesomecam_config_file"

# Payload libraries: dlopen/map/execute by cameraserver.
for so in "$CAMDIR"/*.so; do
  [ -e "$so" ] || continue
  chmod 0644 "$so" 2>/dev/null || true
  chown root:root "$so" 2>/dev/null || true
  chcon u:object_r:system_lib_file:s0 "$so" 2>/dev/null || true
  logi "relabeled $so -> system_lib_file"
done

logi 'post-fs-data done'
