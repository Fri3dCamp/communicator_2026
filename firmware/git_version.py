import subprocess
from packaging.version import Version

Import("env")

def get_firmware_specifier_build_flag():
    ret = subprocess.run(["git", "describe", "--dirty"], stdout=subprocess.PIPE, text=True) #Uses only annotated tags
    #ret = subprocess.run(["git", "describe", "--tags"], stdout=subprocess.PIPE, text=True) #Uses any tags
    build_version = ret.stdout.strip()
    if not build_version:
        build_version = "255.255.255"

    v = Version(build_version)
    build_flag = "-D VERSION_MAJOR=\\\"" + str(v.major) + "\\\" -D VERSION_MINOR=\\\"" + str(v.minor) + "\\\" -D VERSION_PATCH=\\\"" + str(v.micro) + "\\\""
    print(f"Firmware Revision: {v.major}.{v.minor}.{v.micro}")
    return (build_flag)

env.Append(
    BUILD_FLAGS=[get_firmware_specifier_build_flag()]
)
