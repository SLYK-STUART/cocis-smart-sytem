Import("env")
import os, shutil

proj = env["PROJECT_DIR"]
src = os.path.abspath(os.path.join(proj, "..", "common", "cocis_net"))
dst = os.path.join(proj, "components", "cocis_net")
shutil.copytree(src, dst, dirs_exist_ok=True)
print("cocis_net synced from", src)