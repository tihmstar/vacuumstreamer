#!/bin/bash
docker run -v $(pwd):/workspace:Z --rm -it vacuumstreamer $@
