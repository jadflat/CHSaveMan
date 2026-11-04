#!/bin/sh
# The CHGame tools on this project's sketch (Git Bash / Linux): ./cl build, ./cl run ...
here="$(cd "$(dirname "$0")" && pwd)"
exec python "$here/chgame/tools/chgame.py" --sketch "$here/CHSaveMan" "$@"
