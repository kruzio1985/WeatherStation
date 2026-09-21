# ============================================================================
#  Stacja Pogody - obraz LittleFS wezla bez binarek z data\node
#
#  W katalogu data\node leza obrazy firmware'u wezla (4,6 MB) - wydaje je
#  master przez /api/boards, ale na partycji systemu plikow wezla sie nie
#  mieszcza (8 MB plytka: 1,88 MB, C3 4 MB: 0,67 MB). Prob zapisu konczy sie
#  bledem "lfs_write error(-28): File system is full", a PlatformIO konczy
#  budowanie kodem 0 i zapisuje uszkodzony obraz (pliki 0 B).
#
#  PlatformIO nie ma opcji data_dir dla pojedynczego srodowiska, wiec przed
#  budowaniem obrazu kopiujemy data\ bez podkatalogu node do katalogu
#  tymczasowego i podstawiamy go jako PROJECT_DATA_DIR.
#
#  Podlaczenie (srodowiska wezla): extra_scripts = pre:tools/stage_node_fs.py
# ============================================================================
import os
import shutil
import tempfile

Import("env")

data = env.subst("$PROJECT_DATA_DIR")
if os.path.isdir(os.path.join(data, "node")):
    staged = os.path.join(tempfile.gettempdir(), "stacja-www-" + env.subst("$PIOENV"))
    if os.path.isdir(staged):
        shutil.rmtree(staged)
    os.makedirs(staged)
    for name in os.listdir(data):
        if name == "node":
            continue
        src = os.path.join(data, name)
        dst = os.path.join(staged, name)
        if os.path.isdir(src):
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)
    env.Replace(PROJECT_DATA_DIR=staged)
    print("Obraz LittleFS wezla: dane z %s (bez data\\node)" % staged)
