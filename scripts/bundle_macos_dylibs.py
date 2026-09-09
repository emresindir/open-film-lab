#!/usr/bin/env python3
import subprocess
import os
import shutil
import sys

def run(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Error running {cmd}:\n{res.stderr}", file=sys.stderr)
    return res

def get_dependencies(binary_path):
    out = run(["otool", "-L", binary_path]).stdout
    deps = []
    for line in out.splitlines():
        line = line.strip()
        if not line or line.endswith(":"):
            continue
        parts = line.split()
        if parts:
            dep = parts[0]
            if dep.startswith("/opt/homebrew") or dep.startswith("/usr/local") or dep.startswith("@rpath"):
                deps.append(dep)
    return deps

def main():
    if len(sys.argv) < 2:
        print("Usage: bundle_macos_dylibs.py <App.app>")
        sys.exit(1)

    app_path = os.path.abspath(sys.argv[1])
    macos_dir = os.path.join(app_path, "Contents", "MacOS")
    frameworks_dir = os.path.join(app_path, "Contents", "Frameworks")
    os.makedirs(frameworks_dir, exist_ok=True)

    binaries = [os.path.join(macos_dir, f) for f in os.listdir(macos_dir) if os.path.isfile(os.path.join(macos_dir, f))]
    if not binaries:
        print(f"No executable found in {macos_dir}")
        sys.exit(1)

    main_binary = binaries[0]
    print(f"Bundling dependencies for {main_binary}...")

    # Set to track copied dylibs: basename -> original_path
    collected = {}
    queue = []

    # Initial scan of main binary
    for dep in get_dependencies(main_binary):
        if dep.startswith("/") and os.path.exists(dep):
            bname = os.path.basename(dep)
            if bname not in collected:
                collected[bname] = dep
                queue.append((bname, dep))

    # Transitive closure
    while queue:
        bname, src_path = queue.pop(0)
        dst_path = os.path.join(frameworks_dir, bname)
        if not os.path.exists(dst_path):
            shutil.copy2(src_path, dst_path)
            os.chmod(dst_path, 0o755)

        for dep in get_dependencies(src_path):
            if dep.startswith("/") and os.path.exists(dep):
                dep_bname = os.path.basename(dep)
                if dep_bname not in collected:
                    collected[dep_bname] = dep
                    queue.append((dep_bname, dep))

    print(f"Collected {len(collected)} dynamic libraries into Contents/Frameworks/")

    # Update IDs and load commands on all dylibs
    for bname in collected:
        dylib_path = os.path.join(frameworks_dir, bname)
        run(["install_name_tool", "-id", f"@rpath/{bname}", dylib_path])

        for line in run(["otool", "-L", dylib_path]).stdout.splitlines():
            line = line.strip()
            if not line or line.endswith(":"):
                continue
            dep = line.split()[0]
            dep_bname = os.path.basename(dep)
            if dep_bname in collected and dep != f"@rpath/{dep_bname}":
                run(["install_name_tool", "-change", dep, f"@rpath/{dep_bname}", dylib_path])

    # Update main binary
    for dep in get_dependencies(main_binary):
        dep_bname = os.path.basename(dep)
        if dep_bname in collected and dep != f"@rpath/{dep_bname}":
            run(["install_name_tool", "-change", dep, f"@rpath/{dep_bname}", main_binary])

    # Ensure @executable_path/../Frameworks is the sole/primary rpath
    existing_rpaths = []
    otool_out = run(["otool", "-l", main_binary]).stdout
    lines = otool_out.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" in line and i + 2 < len(lines):
            path_line = lines[i+2].strip()
            if path_line.startswith("path "):
                rpath_val = path_line.split()[1]
                existing_rpaths.append(rpath_val)

    for rpath_val in existing_rpaths:
        if rpath_val.startswith("/opt/homebrew") or rpath_val.startswith("/usr/local"):
            run(["install_name_tool", "-delete_rpath", rpath_val, main_binary])

    if "@executable_path/../Frameworks" not in existing_rpaths:
        run(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", main_binary])

    # Ad-hoc sign all dylibs and binary
    print("Codesigning bundled libraries and application...")
    for bname in collected:
        dylib_path = os.path.join(frameworks_dir, bname)
        run(["codesign", "-s", "-", "--force", dylib_path])

    run(["codesign", "-s", "-", "--force", main_binary])
    run(["codesign", "-s", "-", "--force", "--deep", app_path])

    print("✓ Successfully bundled and signed all dynamic libraries!")

if __name__ == "__main__":
    main()
