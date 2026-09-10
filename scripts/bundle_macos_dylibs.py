#!/usr/bin/env python3
import subprocess
import os
import shutil
import sys
import glob

def run(cmd, check=False):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if check and res.returncode != 0:
        print(f"Error running {cmd}:\n{res.stderr}", file=sys.stderr)
    return res

def get_dylib_id(path):
    """Return the LC_ID_DYLIB of a Mach-O file, if present."""
    out = run(["otool", "-D", path]).stdout.splitlines()
    for line in out:
        line = line.strip()
        if line and not line.endswith(":"):
            return line
    return None

def get_rpaths(path):
    """Return all LC_RPATH values from a Mach-O file."""
    out = run(["otool", "-l", path]).stdout
    rpaths = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" in line and i + 2 < len(lines):
            p = lines[i+2].strip()
            if p.startswith("path "):
                rpaths.append(p.split()[1])
    return rpaths

def get_dependencies(binary_path):
    """Return non-system dependency paths for a binary or dylib."""
    out = run(["otool", "-L", binary_path]).stdout
    deps = []
    dylib_id = get_dylib_id(binary_path)
    for line in out.splitlines():
        line = line.strip()
        if not line or line.endswith(":"):
            continue
        parts = line.split()
        if not parts:
            continue
        dep = parts[0]
        # Ignore the dylib's own ID
        if dylib_id and dep == dylib_id:
            continue
        # Ignore Apple system libraries provided by macOS
        if dep.startswith("/usr/lib/") or dep.startswith("/System/"):
            continue
        deps.append(dep)
    return deps

def resolve_dependency(dep, referencing_path, frameworks_dir, macos_dir, brew_prefix):
    """
    Resolve a dependency (which may be an absolute path, @rpath, @loader_path, etc.)
    to a real file on disk.
    """
    # 1. Existing absolute path
    if dep.startswith("/") and os.path.exists(dep):
        return dep

    rel = dep
    if rel.startswith("@rpath/"):
        rel = rel[len("@rpath/"):]
    elif rel.startswith("@loader_path/"):
        rel = rel[len("@loader_path/"):]
    elif rel.startswith("@executable_path/"):
        rel = rel[len("@executable_path/"):]
    else:
        rel = os.path.basename(dep)

    candidates = []

    # 2. Rpaths from referencing binary
    if os.path.exists(referencing_path):
        for rp in get_rpaths(referencing_path):
            bases = [os.path.dirname(referencing_path)]
            try:
                real_dir = os.path.dirname(os.path.realpath(referencing_path))
                if real_dir not in bases:
                    bases.append(real_dir)
            except Exception:
                pass
            for base in bases:
                if "@loader_path" in rp:
                    candidates.append(rp.replace("@loader_path", base))
            if "@executable_path" in rp and macos_dir:
                candidates.append(rp.replace("@executable_path", macos_dir))
            if not rp.startswith("@"):
                candidates.append(rp)

    # 3. Referencing directory itself and its realpath directory
    candidates.append(os.path.dirname(referencing_path))
    try:
        candidates.append(os.path.dirname(os.path.realpath(referencing_path)))
    except Exception:
        pass

    # 4. Standard library directories
    if brew_prefix:
        candidates.append(os.path.join(brew_prefix, "lib"))
    candidates.extend([
        "/opt/homebrew/lib",
        "/usr/local/lib",
        frameworks_dir
    ])

    for d in candidates:
        if d:
            test_path = os.path.normpath(os.path.join(d, rel))
            if os.path.exists(test_path) and os.path.isfile(test_path):
                return test_path

    # 5. Search inside Homebrew opt subtrees
    prefixes = [p for p in [brew_prefix, "/opt/homebrew", "/usr/local"] if p and os.path.exists(p)]
    for prefix in prefixes:
        opt_dir = os.path.join(prefix, "opt")
        if os.path.isdir(opt_dir):
            matches = glob.glob(os.path.join(opt_dir, "*", "lib", rel))
            if matches:
                return matches[0]

    return None

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

    brew_prefix = run(["brew", "--prefix"]).stdout.strip()
    if not brew_prefix:
        brew_prefix = "/opt/homebrew" if os.path.exists("/opt/homebrew") else "/usr/local"

    # Set to track collected dylibs: basename -> original_path
    collected = {}
    queue = []

    # Initial scan of all binaries in Contents/MacOS
    for b in binaries:
        for dep in get_dependencies(b):
            resolved = resolve_dependency(dep, b, frameworks_dir, macos_dir, brew_prefix)
            if resolved:
                bname = os.path.basename(dep) if not dep.startswith("/") else os.path.basename(resolved)
                if bname not in collected:
                    collected[bname] = resolved
                    queue.append((bname, resolved))
            else:
                print(f"Warning: could not resolve dependency '{dep}' for {b}", file=sys.stderr)

    # Transitive closure
    while queue:
        bname, src_path = queue.pop(0)
        dst_path = os.path.join(frameworks_dir, bname)
        if not os.path.exists(dst_path) and not os.path.islink(dst_path):
            shutil.copy2(src_path, dst_path, follow_symlinks=True)
            os.chmod(dst_path, 0o755)

        # Create symlink aliases for different naming variations (e.g. libsharpyuv.0.dylib vs libsharpyuv.dylib)
        try:
            real_src = os.path.realpath(src_path)
            real_bname = os.path.basename(real_src)
            aliases = {real_bname}
            src_dir = os.path.dirname(src_path)
            prefix_pattern = bname.split(".")[0]
            for sibling in glob.glob(os.path.join(src_dir, f"{prefix_pattern}*.dylib")):
                if os.path.realpath(sibling) == real_src:
                    aliases.add(os.path.basename(sibling))

            for alias in aliases:
                if alias != bname:
                    alias_dst = os.path.join(frameworks_dir, alias)
                    if not os.path.exists(alias_dst) and not os.path.islink(alias_dst):
                        os.symlink(bname, alias_dst)
                    collected[alias] = real_src
        except Exception as e:
            print(f"Note: error creating aliases for {bname}: {e}", file=sys.stderr)

        # Scan dependencies of this library
        for dep in get_dependencies(src_path):
            resolved = resolve_dependency(dep, src_path, frameworks_dir, macos_dir, brew_prefix)
            if resolved:
                dep_bname = os.path.basename(dep) if not dep.startswith("/") else os.path.basename(resolved)
                if dep_bname not in collected:
                    collected[dep_bname] = resolved
                    queue.append((dep_bname, resolved))
            else:
                print(f"Warning: could not resolve dependency '{dep}' for {src_path}", file=sys.stderr)

    print(f"Collected {len(collected)} dynamic library entries into Contents/Frameworks/")

    # Update IDs, LC_RPATHs, and load commands on all dylibs
    framework_files = [f for f in os.listdir(frameworks_dir) if os.path.isfile(os.path.join(frameworks_dir, f))]
    for f in sorted(framework_files):
        dylib_path = os.path.join(frameworks_dir, f)
        if os.path.islink(dylib_path):
            continue

        os.chmod(dylib_path, 0o755)
        # Set ID
        run(["install_name_tool", "-id", f"@rpath/{f}", dylib_path])

        # Ensure @loader_path is present and purge stale rpaths
        dylib_rpaths = get_rpaths(dylib_path)
        for rp in dylib_rpaths:
            if rp.startswith("/opt/homebrew") or rp.startswith("/usr/local") or rp.startswith("@loader_path/.."):
                run(["install_name_tool", "-delete_rpath", rp, dylib_path])
        if "@loader_path" not in dylib_rpaths:
            run(["install_name_tool", "-add_rpath", "@loader_path", dylib_path])

        # Rewrite load commands
        for line in run(["otool", "-L", dylib_path]).stdout.splitlines():
            line = line.strip()
            if not line or line.endswith(":"):
                continue
            parts = line.split()
            if not parts:
                continue
            dep = parts[0]
            dep_bname = os.path.basename(dep)
            if dep_bname in collected:
                target_rpath = f"@rpath/{dep_bname}"
                if dep != target_rpath:
                    run(["install_name_tool", "-change", dep, target_rpath, dylib_path])

    # Update main binaries
    for b in binaries:
        os.chmod(b, 0o755)
        for line in run(["otool", "-L", b]).stdout.splitlines():
            line = line.strip()
            if not line or line.endswith(":"):
                continue
            parts = line.split()
            if not parts:
                continue
            dep = parts[0]
            dep_bname = os.path.basename(dep)
            if dep_bname in collected:
                target_rpath = f"@rpath/{dep_bname}"
                if dep != target_rpath:
                    run(["install_name_tool", "-change", dep, target_rpath, b])

        # Ensure @executable_path/../Frameworks is present and stale rpaths removed
        bin_rpaths = get_rpaths(b)
        for rpath_val in bin_rpaths:
            if rpath_val.startswith("/opt/homebrew") or rpath_val.startswith("/usr/local"):
                run(["install_name_tool", "-delete_rpath", rpath_val, b])

        if "@executable_path/../Frameworks" not in bin_rpaths:
            run(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", b])

    # Ad-hoc sign all dylibs and binaries
    print("Codesigning bundled libraries and application...")
    for f in sorted(framework_files):
        dylib_path = os.path.join(frameworks_dir, f)
        if os.path.isfile(dylib_path) and not os.path.islink(dylib_path):
            run(["codesign", "-s", "-", "--force", dylib_path])

    for b in binaries:
        run(["codesign", "-s", "-", "--force", b])

    run(["codesign", "-s", "-", "--force", "--deep", app_path])

    print("✓ Successfully bundled and signed all dynamic libraries!")

if __name__ == "__main__":
    main()

