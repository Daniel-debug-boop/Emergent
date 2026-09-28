#!/usr/bin/env bash
# Negative controls for the render graph's Vulkan binding layer.
#
# Same discipline as render_graph_controls.sh: a suite that has never been
# shown to fail is not evidence. Each mutation below breaks one specific
# behaviour the suite claims to cover, and the control only passes if the suite
# notices. A mutation that survives means the corresponding test is not testing
# what its name says.
#
# The source is restored after every mutation, so this is safe to run against a
# dirty tree -- it only ever edits render_graph_vulkan.cpp and puts it back.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/native/src/render_graph_vulkan.cpp"

BUILD="${EMERGENT_BUILD_DIR:-$ROOT/build/native}"
BACKUP="$(mktemp)"
TEST_BIN="$BUILD/emergent_render_graph_vulkan_test"

if [ ! -x "$TEST_BIN" ]; then
    echo "no test binary at $TEST_BIN"
    echo "build it first, or set EMERGENT_BUILD_DIR to the configured build tree"
    exit 1
fi

cp "$SRC" "$BACKUP"
restore() { cp "$BACKUP" "$SRC"; rm -f "$BACKUP"; }
trap restore EXIT

failures=0

mutate() {
    local from="$1" to="$2"
    python3 - "$SRC" "$from" "$to" <<'PY'
import io, sys
path, frm, to = sys.argv[1], sys.argv[2], sys.argv[3]
s = io.open(path, encoding='utf-8').read()
if frm not in s:
    sys.exit(3)
io.open(path, 'w', encoding='utf-8').write(s.replace(frm, to, 1))
PY
}

run_mutation() {
    local name="$1" from="$2" to="$3"
    cp "$BACKUP" "$SRC"
    if ! mutate "$from" "$to"; then
        printf 'SKIP  %-46s (mutation site not found -- test the harness)\n' "$name"
        failures=$((failures + 1))
        return
    fi
    if ! cmake --build "$BUILD" --target emergent_render_graph_vulkan_test -j2 >/tmp/rgv_build.log 2>&1; then
        printf 'CAUGHT %-45s (build failed)\n' "$name"
        return
    fi
    if "$TEST_BIN" >/tmp/rgv_test.log 2>&1; then
        printf 'SURVIVED %-43s <-- the suite did not notice\n' "$name"
        failures=$((failures + 1))
    else
        printf 'CAUGHT %-45s (%s)\n' "$name" "$(grep -c '^FAIL' /tmp/rgv_test.log) assertions failed)"
    fi
}

# A change that alters behaviour but nothing the suite claims, to prove the
# controls are not simply failing on every edit.
run_cosmetic() {
    local name="$1" from="$2" to="$3"
    cp "$BACKUP" "$SRC"
    if ! mutate "$from" "$to"; then
        printf 'SKIP  %-46s (site not found)\n' "$name"
        failures=$((failures + 1))
        return
    fi
    if ! cmake --build "$BUILD" --target emergent_render_graph_vulkan_test -j2 >/tmp/rgv_build.log 2>&1; then
        printf 'SURVIVED %-43s <-- a cosmetic edit broke the build\n' "$name"
        failures=$((failures + 1))
        return
    fi
    if "$TEST_BIN" >/tmp/rgv_test.log 2>&1; then
        printf 'OK     %-45s (cosmetic change, correctly ignored)\n' "$name"
    else
        printf 'SURVIVED %-43s <-- a cosmetic edit failed the suite\n' "$name"
        failures=$((failures + 1))
    fi
}

echo "render graph vulkan binding negative controls"
echo

# -- the format table -------------------------------------------------------
run_mutation "srgb mapped to the unorm format" \
    "{Format::R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, 4" \
    "{Format::R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, 4"

run_mutation "two formats collapsed onto one VkFormat" \
    "{Format::R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8" \
    "{Format::R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, 8"

run_mutation "d24s8 loses its stencil aspect" \
    "VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT" \
    "VK_IMAGE_ASPECT_DEPTH_BIT"

run_mutation "texel size of rgba16f halved" \
    "{Format::R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8" \
    "{Format::R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 4"

# -- usage translation ------------------------------------------------------
# The bug this layer exists to prevent: treating the graph's usage bits as if
# they were Vulkan's.
run_mutation "sampled bit stops translating" \
    "if (declared & TEXTURE_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;" \
    "if (declared & TEXTURE_USAGE_STORAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;"

run_mutation "storage bit stops translating" \
    "if (declared & TEXTURE_USAGE_STORAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;" \
    "if (false) usage |= VK_IMAGE_USAGE_STORAGE_BIT;"

run_mutation "indirect buffer usage lost" \
    "if (declared & BUFFER_USAGE_INDIRECT_BIT) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;" \
    "if (false) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;"

run_mutation "an undeclared colour image loses its attachment bit" \
    "usage |= isDepthStencilFormat(dim.format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                  : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;" \
    "usage |= isDepthStencilFormat(dim.format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                  : 0;"

run_mutation "a mip chain stops being a transfer destination" \
    "usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;" \
    "usage |= VK_IMAGE_USAGE_SAMPLED_BIT;"

# -- load / store -----------------------------------------------------------
run_mutation "load always happens" \
    "loadOp = needsLoad ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;" \
    "loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;"

run_mutation "store is always DONT_CARE" \
    "storeOp = needsStore ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;" \
    "storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;"

# -- layouts ----------------------------------------------------------------
run_mutation "transient resources no longer get GENERAL" \
    "if (isTransient) return VK_IMAGE_LAYOUT_GENERAL;" \
    "if (false) return VK_IMAGE_LAYOUT_GENERAL;"

run_mutation "a depth target is laid out as colour" \
    "return isDepthStencilFormat(dim.format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL" \
    "return false ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL"

run_mutation "a sampled colour target keeps its attachment layout" \
    "    if (isDepthStencilFormat(dim.format)) return VK_IMAGE_LAYOUT_GENERAL;
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;" \
    "    if (isDepthStencilFormat(dim.format)) return VK_IMAGE_LAYOUT_GENERAL;
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;"

# -- descriptor layout ------------------------------------------------------
run_mutation "descriptor layout accepts an unbaked graph" \
    "        throw std::runtime_error(
            \"deriveDescriptorLayout: the graph has not been baked, so its resource set is incomplete\");" \
    "        return plan;"

run_mutation "every texture gets the same binding number" \
    "b.binding = nextTexture++;" \
    "b.binding = 0;"

run_mutation "sampled textures become storage images" \
    "b.type = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                             : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;" \
    "b.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            (void)storage;"

# -- barriers ---------------------------------------------------------------
run_mutation "the first write no longer starts from UNDEFINED" \
    "const VkImageLayout have = it == currentLayout.end() ? VK_IMAGE_LAYOUT_UNDEFINED : it->second;" \
    "const VkImageLayout have = it == currentLayout.end() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : it->second;"

run_mutation "no-op transitions are still emitted" \
    "if (have == want) return;" \
    "if (false) return;"

run_mutation "the layout is never carried forward" \
    "currentLayout[phys] = want;" \
    ""

echo
run_cosmetic "no false positive on a cosmetic edit" \
    "return entry ? entry->vk : VK_FORMAT_UNDEFINED;" \
    "return entry != nullptr ? entry->vk : VK_FORMAT_UNDEFINED;"

echo
if [ "$failures" -eq 0 ]; then
    echo "all negative controls behaved as expected"
    exit 0
fi
echo "$failures control(s) did not behave as expected"
exit 1
