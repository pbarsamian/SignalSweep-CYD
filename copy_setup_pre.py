Import("env")
import shutil, glob, os

src = os.path.join("config", "User_Setup.h")
pattern = os.path.join(".pio", "libdeps", "*", "TFT_eSPI", "User_Setup.h")
matches = glob.glob(pattern)
if matches:
    for dst in matches:
        shutil.copy(src, dst)
        print(f"[pre-build] Copied User_Setup.h -> {dst}")
else:
    print("[pre-build] TFT_eSPI not yet downloaded, skipping User_Setup.h copy")
