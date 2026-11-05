"""Makes the README pictures: rebuilds the public card, runs tools/scripts/pictures.txt on it and, with a folder
given, copies the published shots there. SAVM_BAK stays unset, so the simulator starts from a fresh backup file.
    python CHSaveMan/tools/pictures.py [<clean worktree>/docs/shots]
Two runs give identical files, so `git diff` on docs/shots shows any picture that changed."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CARD = ROOT / "out" / "pubcard"
OUT = ROOT / "CHSaveMan" / "out" / "pictures"
SHOTS = ["01_main", "02_page_menu", "03_backup_chooser", "05_scan_done", "06_games_list", "09_details"]

subprocess.run([sys.executable, str(ROOT / "CHSaveMan" / "tools" / "make_pubcard.py")], check=True)
if not (CARD / "GAMES").is_dir():
    sys.exit("no GAMES folder in " + str(CARD))
env = dict(os.environ, SAVM_CARD=str(CARD).replace("\\", "/"))
env.pop("SAVM_BAK", None)
shutil.rmtree(OUT, ignore_errors=True)
cl = "cl.cmd" if os.name == "nt" else "./cl"
subprocess.run([str(ROOT / cl) if os.name == "nt" else cl, "run", "CHSaveMan/tools/scripts/pictures.txt", "out/pictures"],
               check=True, cwd=ROOT, env=env)
for s in SHOTS:
    if not (OUT / (s + ".png")).is_file():
        sys.exit("missing shot " + s)
if len(sys.argv) > 1:
    for s in SHOTS:
        shutil.copy(OUT / (s + ".png"), sys.argv[1])
print("ok:", OUT)
