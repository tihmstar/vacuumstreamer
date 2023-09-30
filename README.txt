To setup:
docker build -t vacuumstramer .
./run.sh make

On the target run:
LD_PRELOAD=/data/vacuumstreamer.so video_monitor

for persistance add the line to /mnt/misc/_root_postboot.sh

Robot will have an open port on 6969. To retrieve video from your desktop:

nc <robotip> 6969 | ffmpeg -i - -f mpeg - | vlc -

MacOS:
nc <robotip> 6969 | ffmpeg -i - -f mpeg - | /Applications/VLC.app/Contents/MacOS/VLC -

