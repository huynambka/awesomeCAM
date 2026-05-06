#!/system/bin/sh
CAMDIR=/data/camera
LOGTAG='sepolicy-patcher'

chcon u:object_r:system_data_file:s0 "$CAMDIR" 2>/dev/null || true

if [ -f "$CAMDIR/input.mp4" ]; then
  chcon u:object_r:awesomecam_source_file:s0 "$CAMDIR/input.mp4" 2>/dev/null || true
  chmod 0644 "$CAMDIR/input.mp4" 2>/dev/null || true
fi

touch "$CAMDIR/awesomecam_offsets.conf" 2>/dev/null || true
chcon u:object_r:awesomecam_config_file:s0 "$CAMDIR/awesomecam_offsets.conf" 2>/dev/null || true
chmod 0666 "$CAMDIR/awesomecam_offsets.conf" 2>/dev/null || true

for so in "$CAMDIR"/*.so; do
  [ -e "$so" ] || continue
  chcon u:object_r:system_lib_file:s0 "$so" 2>/dev/null || true
  chmod 0644 "$so" 2>/dev/null || true
done

log -t "$LOGTAG" -p i 'manual relabel done' 2>/dev/null || true
