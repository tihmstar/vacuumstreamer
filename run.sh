#!/bin/bash
docker run -v $(pwd):/workspace --rm -it vacuum $@
