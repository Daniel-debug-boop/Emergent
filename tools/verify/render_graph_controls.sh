#!/usr/bin/env bash
# Negative controls for the render graph.
#
# A test suite that has never been shown to fail is not evidence. Each mutation
# below breaks one specific behaviour the suite claims to cover; the control only
# passes if the suite notices. A mutation that survives means the corresponding
# test is not testing what its name says, and the name is worse than no test
# because it reads like coverage.
#
# The source is restored after every mutation, so this script is safe to run
# against a dirty tree -- it only ever edits render_graph.cpp and puts it back.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/native/src/render_graph.cpp"

# Where the test binary lives. Overridable because CI configures into
# build/native while a local convenience build uses build-native, and a harness
# that only works against one of them is a harness that silently does nothing on
# the other.
BUILD="${EMERGENT_BUILD_DIR:-$ROOT/build-native}"
BACKUP="$(mktemp)"
TEST_BIN="$BUILD/emergent_render_graph_test"

if [ ! -x "$TEST_BIN" ]; then
    echo "no test binary at $TEST_BIN"
    echo "build it first, or set EMERGENT_BUILD_DIR to the configured build tree"
    exit 1
fi

cp "$SRC" "$BACKUP"
restore() { cp "$BACKUP" "$SRC"; rm -f "$BACKUP"; }
trap restore EXIT

failures=0

# Replace the first occurrence of a literal, and confirm the file changed.
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
    local name="$1" expect="$2" from="$3" to="$4"
    cp "$BACKUP" "$SRC"
    if ! mutate "$from" "$to"; then
        printf 'SKIP  %-46s (mutation site not found -- test the harness)\n' "$name"
        failures=$((failures + 1))
        return
    fi
    if ! cmake --build "$BUILD" --target emergent_render_graph_test -j2 >/tmp/rg_build.log 2>&1; then
        printf 'CAUGHT %-45s (build failed)\n' "$name"
        return
    fi
    local out
    out="$("$TEST_BIN" 2>&1)"
    local rc=$?
    if [ "$expect" = "fail" ] && [ $rc -ne 0 ]; then
        local n
        n="$(printf '%s\n' "$out" | grep -c '  FAIL ' || true)"
        printf 'CAUGHT %-45s (%d assertions failed)\n' "$name" "$n"
    elif [ "$expect" = "pass" ]; then
        printf 'OK     %-45s (behaviour unchanged, as expected)\n' "$name"
    else
        printf 'MISSED %-45s <-- the suite did not notice\n' "$name"
        printf '%s\n' "$out" | tail -5
        failures=$((failures + 1))
    fi
}

echo "render graph negative controls"
echo

# 1. A swapchain-relative target ignores its height fraction.
run_mutation "SwapchainRelative ignores size_y" fail \
    'dim.height = scaledExtent(info.size_y, backbufferDimensions_.height);' \
    'dim.height = backbufferDimensions_.height; // mutation: height no longer scaled'

# 2. The min-one extent clamp is removed, so a vanishing fraction allocates nothing.
run_mutation "extent clamp to one removed" fail \
    'if (!(scaled > 0.0)) {
        return 1;
    }' \
    'if (!(scaled > 0.0)) {
        return 0;
    }'

# 3. The full mip chain is off by one.
run_mutation "mip chain count off by one" fail \
    'uint32_t levels = 0;
    while (maxDim != 0) {
        levels++;
        maxDim >>= 1;
    }' \
    'uint32_t levels = 0;
    while (maxDim > 1) {
        levels++;
        maxDim >>= 1;
    }'

# 4. A resource read by a second pass is wrongly eligible for transience.
#    This is the control that matters most: it is the one that would destroy a
#    target a later pass is about to read, and it produces a plausible image
#    rather than a crash.
run_mutation "cross-pass resource allowed to be transient" fail \
    'for (uint32_t pass : res->getReadPasses()) {' \
    'for (uint32_t pass : std::vector<uint32_t>{}) { // mutation: reads ignored'

# 5. Buffers become eligible for transience.
run_mutation "buffers allowed to be transient" fail \
    'if (dim.isBufferLike()) {
            dim.flags &= ~ATTACHMENT_INFO_INTERNAL_TRANSIENT_BIT;
            continue;
        }' \
    'if (false) {
            continue;
        }'

# 6. A history read no longer disqualifies transience.
run_mutation "history no longer forces persistence" fail \
    'if (index < physicalImageHasHistory_.size() && physicalImageHasHistory_[index]) {' \
    'if (false) {'

# 7. Two passes writing one target are no longer ordered against each other.
run_mutation "write-after-write ordering removed" fail \
    'dependencies_.push_back(PassDependency{later, earlier, PassDependency::Kind::WriteAfterWrite});' \
    '(void)earlier; (void)later;'

# 8. Subpass aliasing is disabled, so merging never collapses a physical slot.
run_mutation "subpass aliasing disabled" fail \
    'joinGroups(in->getIndex(), out->getIndex());' \
    '(void)in; (void)out;'

# 9. Reading a resource nothing writes stops being an error.
run_mutation "read-without-write accepted" fail \
    'if (writers.empty() && !readers.empty()) {
            throw std::logic_error("resource '"'"'" + res->getName() + "'"'"' is read but never written");
        }' \
    'if (writers.empty() && !readers.empty()) {
            continue; // mutation: unwritten read accepted
        }'

# 10. A cycle stops being detected, so the scheduler silently drops passes.
run_mutation "dependency cycles accepted" fail \
    'throw std::logic_error("render graph has a dependency cycle among passes: " + names);' \
    'names.clear(); break;'

# 11. A control that must NOT be caught: a cosmetic change that touches no
#     behaviour under test. If the suite fails here it is over-specified.
cp "$BACKUP" "$SRC"
mutate 'std::string out;
    out += "render graph: "' 'std::string out;
    out += "render graph (v2): "'
if cmake --build "$BUILD" --target emergent_render_graph_test -j2 >/tmp/rg_build.log 2>&1; then
    if "$TEST_BIN" >/dev/null 2>&1; then
        printf 'OK     %-45s (cosmetic change, correctly ignored)\n' "no false positive on a cosmetic edit"
    else
        printf 'MISSED %-45s <-- suite is over-specified\n' "no false positive on a cosmetic edit"
        failures=$((failures + 1))
    fi
else
    printf 'CAUGHT %-45s (build failed)\n' "no false positive on a cosmetic edit"
fi

cp "$BACKUP" "$SRC"
echo
if [ "$failures" -eq 0 ]; then
    echo "all negative controls behaved as expected"
    exit 0
fi
echo "$failures control(s) did not behave as expected"
exit 1
