# vacuumstreamer

This is a project to stream video from the Ava vacuum cleaner using the `video_monitor` binary and
`go2rtc`.

Tested on: Dreame L10s Ultra.

## Build

```bash
docker build -t vacuumstreamer .
./run.sh make
```

## Install vacuumstreamer

Copy the `vacuumstreamer.so`, `video_monitor`, and configuration files to the vacuum robot:

```bash
ssh root@${VACUUM_ROBOT_IP} "mkdir -p /data/vacuumstreamer"
scp -O vacuumstreamer.so root@${VACUUM_ROBOT_IP}:/data/vacuumstreamer/vacuumstreamer.so
scp -O dist/usr/bin/video_monitor root@${VACUUM_ROBOT_IP}:/data/vacuumstreamer/video_monitor
scp -Or dist/ava/conf/video_monitor/ root@${VACUUM_ROBOT_IP}:/data/vacuumstreamer/ava_conf_video_monitor
```

## Install go2rtc

Run on the vacuum robot:

```bash
curl -L https://github.com/AlexxIT/go2rtc/releases/download/v1.9.9/go2rtc_linux_arm64 -o /data/vacuumstreamer/go2rtc
chmod +x /data/vacuumstreamer/go2rtc
```

## Persistent configuration

On the vacuum robot, run the following commands to configure vacuumstreamer on startup:

```bash
# workaround for missing certificate bug, see https://github.com/tihmstar/vacuumstreamer/issues/1 for details
cp -r /mnt/private /data/vacuumstreamer/mnt_private_copy && touch /data/vacuumstreamer/mnt_private_copy/certificate.bin

cat <<EOF >> /data/_root_postboot.sh

if [[ -f /data/vacuumstreamer/video_monitor ]]; then
    mount --bind /data/vacuumstreamer/ava_conf_video_monitor /ava/conf/video_monitor
    mount --bind /data/vacuumstreamer/mnt_private_copy /mnt/private
    LD_PRELOAD=/data/vacuumstreamer/vacuumstreamer.so /data/vacuumstreamer/video_monitor > /dev/null 2>&1 &
    /data/vacuumstreamer/go2rtc -c '{"streams": {"tcp_magic": "tcp://127.0.0.1:6969"}}' > /dev/null 2>&1 &
fi
EOF
```

Credits to @Uberi, source: https://anthony-zhang.me/blog/offline-robot-vacuum/
