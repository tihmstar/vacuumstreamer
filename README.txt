To setup:
docker build -t vacuumstramer .
./run.sh make

On the target run:
LD_PRELOAD=/data/vacuumstreamer.so video_monitor

for persistance add the line to /data/_root_postboot.sh
Dustbuilder installs the vacuumstreamer.so by default in /usr/lib
Robot will have an open port on 6969. To retrieve video from your desktop:

nc <robotip> 6969 | ffmpeg -i - -f mpeg - | vlc -

MacOS:
nc <robotip> 6969 | ffmpeg -i - -f mpeg - | /Applications/VLC.app/Contents/MacOS/VLC -

