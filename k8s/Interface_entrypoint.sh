#!/bin/bash
set -e
./bin/Hello ./conf/Interface.json
EXIT=$?
if [ $EXIT -ne 0 ]; then echo "FATAL: exited with code $EXIT"; exit $EXIT; fi
tail -f log/Interface_robot.log 2>/dev/null &
sleep infinity
