#!/usr/bin/env bash
# Build a ps5-opengl runtime with small render-target textures on the
# linear+staging path, and install it into build/ps5-opengl-staging/prefix.
#
# Run inside the pokemmo-ps5-buildenv container with the port checkout at /work:
#   docker run --rm -v <port>:/work -w /work pokemmo-ps5-buildenv:latest \
#       bash scripts/build-ps5-opengl-staging.sh
#
# The stock SDK pads every native color render target to 2 MiB
# (PS5_RENDER_ALIGNMENT).  PokeMMO creates a render target for every texture
# it uploads, so a few hundred small sprites exhaust direct memory.  The
# linear+staging path is already used for mipmapped render/sample textures;
# route small single-level ones through it too.
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
stage="$root/build/ps5-opengl-staging"
src="$stage/src"
objdir="$src/build/core33-native-runtime"
prefix="$stage/prefix"
patch_file="$root/scripts/patches/ps5-opengl-small-texture-staging.patch"
sdk=${PS5_PAYLOAD_SDK:-/opt/ps5/payload-sdk}
sdk_prefix=${PS5_OPENGL_PREFIX:-/opt/ps5/opengl}

mkdir -p "$stage"
if [[ ! -f "$src/.prepared" ]]; then
    echo "== copy ps5-opengl $(cat "$sdk_prefix/share/ps5-opengl/ps5-opengl.mk" 2>/dev/null | head -1 || true)"
    rm -rf "$src"
    mkdir -p "$src"
    cp -a /opt/ps5/opengl-src/. "$src/"
    echo "== apply small-texture staging patch"
    python3 - "$src/src/gallium/ps5/ps5_screen.c" <<'PY'
import sys
from pathlib import Path
path = Path(sys.argv[1])
text = path.read_text()
anchor = "static bool\nps5_linear_sampled_layout(const struct pipe_resource *resource)\n{"
addition = """/* Native color render targets are padded to PS5_RENDER_ALIGNMENT (2 MiB).
 * A 512x512 RGBA8 sprite is 1 MiB, so the direct path burns a 2 MiB block for
 * every small texture the client keeps.  The linear+staging path already
 * handles mipmapped render/sample textures; use it whenever the direct block
 * would waste more than half of its space. */
#define PS5_LINEAR_STAGING_MAX_BYTES (PS5_RENDER_ALIGNMENT / 2u)

"""
old = """       (resource->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW)) ==
          (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW))
      return false;"""
new = """       (resource->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW)) ==
          (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW) &&
       (uint64_t)resource->width0 * resource->height0 *
             util_format_get_blocksize(resource->format) >
          PS5_LINEAR_STAGING_MAX_BYTES)
      return false;"""
if text.count(anchor) != 1 or text.count(old) != 1:
    raise SystemExit("ps5_screen.c patch anchors not unique")
text = text.replace(anchor, addition + anchor, 1).replace(old, new, 1)
banner = """   printf("[ps5-cache] clflushopt=%u cacheline=%u\\n",
          util_get_cpu_caps()->has_clflushopt, util_get_cpu_caps()->cacheline);
"""
banner_new = banner + """   printf("[ps5-gallium] small-rt-staging max-bytes=%u alignment=%u\\n",
          PS5_LINEAR_STAGING_MAX_BYTES, PS5_RENDER_ALIGNMENT);
"""
if text.count(banner) != 1:
    raise SystemExit("ps5_screen.c banner anchor not unique")
text = text.replace(banner, banner_new, 1)
path.write_text(text)
print("patched", path)
PY
    touch "$src/.prepared"
else
    echo "== source tree already prepared: $src"
fi

cd "$src"

# The buildenv ships the PS5 SDK and ninja but not Meson; build it in a venv
# under the untracked build directory.
if ! command -v meson >/dev/null 2>&1; then
    if [[ ! -x "$stage/venv/bin/meson" ]]; then
        echo "== create Meson virtual environment"
        python3 -m venv "$stage/venv"
        "$stage/venv/bin/pip" install --quiet meson packaging mako pyyaml setuptools
    fi
    # Mesa's meson.build requires either distutils (removed in Python 3.12)
    # or the packaging module, and Mako/PyYAML for its generated sources.  The
    # venv does not inherit system packages.
    "$stage/venv/bin/python" -c 'import packaging, mako, yaml' 2>/dev/null ||
        "$stage/venv/bin/pip" install --quiet packaging mako pyyaml setuptools
    export PATH="$stage/venv/bin:$PATH"
fi

if [[ ! -f third_party/mesa-26.2.0/VERSION ]]; then
    echo "== fetch Mesa 26.2.0 and pinned PSBC sources"
    python3 tools/fetch-sources.py
fi

# The buildenv has git but not the GNU patch utility.  Mesa's integration
# script only needs a unified-diff applier, and git apply provides that.
mkdir -p "$stage/bin"
cat > "$stage/bin/patch" <<'EOF'
#!/usr/bin/env bash
dir=.
strip=1
while (($#)); do
    case "$1" in
        -d) dir=$2; shift 2 ;;
        -p*) strip=${1#-p}; shift ;;
        --batch|--forward|-N|-f) shift ;;
        *) shift ;;
    esac
done
cd "$dir" || exit 1
# --unidiff-zero: the Mesa patch has a few single-line hunks without context.
# --ignore-space-change: one st_extensions.c hunk has hand-adjusted indent.
exec git apply -p"$strip" --unidiff-zero --ignore-space-change --whitespace=nowarn
EOF
chmod +x "$stage/bin/patch"
export PATH="$stage/bin:$PATH"

if [[ ! -f build/mesa-ps5-probe/build.ninja ]]; then
    # A failed first configure leaves meson-private behind and makes a plain
    # `meson setup` refuse to reuse the directory.
    if [[ -d build/mesa-ps5-probe ]]; then
        echo "== removing incomplete Mesa build directory"
        rm -rf build/mesa-ps5-probe
    fi
    echo "== configure and build Mesa core archives"
    PS5_PAYLOAD_SDK="$sdk" PS5_MESA_CROSS_FILE="$(find "$sdk/toolchain" -maxdepth 1 -name "*.ini" | head -1)" \
        bash toolchain/build-mesa-ps5.sh
else
    echo "== Mesa core build already present"
fi

# The runtime object rule needs the PSBC headers and archive; the archive is
# already in the stock SDK.  We do not rebuild the shader compiler.
if [[ ! -f third_party/opengnm-psbc/libpsbc.ps5.a ]]; then
    cp "$sdk_prefix/lib/libpsbc.ps5.a" third_party/opengnm-psbc/libpsbc.ps5.a
fi

# PSBC's headers include files that only its Meson build generates
# (builtin_types.h, nir_opcodes.h, the nir_intrinsics headers,
# nir_builder_opcodes.h).  The stock SDK ships libpsbc.ps5.a but not those
# headers, and we skip the PSBC build, so generate them here from PSBC's own
# scripts, which match the Mesa revision its archive was built from.
psbc=third_party/opengnm-psbc
generated=(src/compiler/builtin_types.h src/compiler/builtin_types.c
           src/compiler/nir/nir_opcodes.h src/compiler/nir/nir_intrinsics.h
           src/compiler/nir/nir_intrinsics_indices.h src/compiler/nir/nir_builder_opcodes.h)
missing=()
for file in "${generated[@]}"; do
    [[ -f "$psbc/$file" ]] || missing+=("$file")
done
py=python3
if [[ -x "$stage/venv/bin/python" ]]; then
    py="$stage/venv/bin/python"
fi
if ((${#missing[@]})); then
    echo "== generate PSBC headers: ${missing[*]}"
    (
        cd "$psbc"
        if [[ ! -f src/compiler/builtin_types.h ]]; then
            "$py" src/compiler/builtin_types_h.py src/compiler/builtin_types.h
            "$py" src/compiler/builtin_types_c.py src/compiler/builtin_types.c
        fi
        cd src/compiler/nir
        if [[ ! -f nir_opcodes.h ]]; then
            "$py" nir_opcodes_h.py > nir_opcodes.h
        fi
        if [[ ! -f nir_intrinsics.h ]]; then
            "$py" nir_intrinsics_h.py --out nir_intrinsics.h
        fi
        if [[ ! -f nir_intrinsics_indices.h ]]; then
            "$py" nir_intrinsics_indices_h.py --out nir_intrinsics_indices.h
        fi
        if [[ ! -f nir_builder_opcodes.h ]]; then
            "$py" nir_builder_opcodes_h.py > nir_builder_opcodes.h
        fi
    )
fi

# The AMD register headers are generated too; PSBC's own Makefile has the exact
# recipe (including the SQ export defines appended to amdgfxregs.h).
amd_generated=(src/amd/common/amdgfxregs.h src/amd/common/sid_tables.h)
missing_amd=()
for file in "${amd_generated[@]}"; do
    [[ -f "$psbc/$file" ]] || missing_amd+=("$file")
done
if ((${#missing_amd[@]})); then
    echo "== generate PSBC AMD headers: ${missing_amd[*]}"
    make -C "$psbc" -f Makefile CONFIG="$src/toolchain/opengnm-psbc-host.mak" \
        PYTHON="$py" "${missing_amd[@]}"
fi

# Build the whole runtime, not just ps5_screen.o.  The SDK archive was built with
# PS5_DYNAMIC_SCANOUT (its prefix ships ps5_opengl_display_modes.h), and its
# objects share one flag set; swapping a single object built without that define
# links but boots into a different display path (SIGSYS before the log opens).
echo "== build patched runtime (dynamic scanout, SDK feature set)"
rm -rf "$objdir"
make -C tests/ps5 -f native-app.mk -j"$(nproc)" runtime \
    PS5_PAYLOAD_SDK="$sdk" \
    PS5_OPENGL_ROOT="$src" \
    PS5_OPENGL_PSBC="$src/third_party/opengnm-psbc" \
    PS5_DYNAMIC_SCANOUT=1 PS5_SCANOUT_HEIGHT=2160 PS5_SCANOUT_FPS=120

for lib in libps5_opengl_core33.a libSceAgc.so libSceAgcDriver.so; do
    [[ -s "$objdir/$lib" ]] || { echo "missing $objdir/$lib" >&2; exit 2; }
done
grep -q 'small-rt-staging' <(strings "$objdir/libps5_opengl_core33.a")
echo "== patched marker present in runtime archive"

echo "== install runtime into $prefix"
rm -rf "$prefix"
mkdir -p "$prefix"
# $sdk_prefix is itself a symlink (/opt/ps5/opengl -> opengl-bundle/sdk), so copy
# the directory's contents; a plain `cp -a` would copy the link and leave the
# prefix pointing into the read-only image.
cp -a "$sdk_prefix/." "$prefix/"
for lib in libps5_opengl_core33.a libSceAgc.so libSceAgcDriver.so; do
    install -m 0644 "$objdir/$lib" "$prefix/lib/$lib"
done
cp "$objdir/ps5_screen.o" "$stage/ps5_screen.o"
# Same manifest recipe as ps5-opengl's install-ps5-opengl-core33.sh, regenerated
# because three archives changed.  build-title.sh verifies it before linking.
(
    cd "$prefix"
    find include lib share -type f -print0 | sort -z | xargs -0 sha256sum
) > "$prefix/manifest.sha256"
sha256sum "$stage/ps5_screen.o" "$prefix/lib/libps5_opengl_core33.a"
grep 'lib/libps5_opengl_core33.a' "$prefix/manifest.sha256"
echo "== done: $prefix"
