#!/usr/bin/env bash
# Negative controls for the adaptive visibility system.
#
# A test suite that has never been shown to fail is not evidence. Each mutation
# below breaks one specific behaviour the suite claims to cover; the control
# only passes if the suite notices. A mutation that survives means the
# corresponding test is not testing what its name says, and the name is worse
# than no test because it reads like coverage.
#
# Every mutation here targets the *correctness invariant* rather than a
# convenient internal: a conservative reject that has stopped being
# conservative, a cache that has stopped invalidating, a cone test that has
# started rejecting on invalid input. Those are the bugs this module exists to
# not have, so they are the bugs worth proving it catches.
#
# The source is restored after every mutation, so this script is safe to run
# against a dirty tree -- it only ever edits adaptive.cpp and puts it back.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/native/src/adaptive.cpp"

# Overridable because CI configures into build/native while a local convenience
# build uses build-native, and a harness that only works against one of them is
# a harness that silently does nothing on the other.
BUILD="${EMERGENT_BUILD_DIR:-$ROOT/build-native}"
BACKUP="$(mktemp)"
TEST_BIN="$BUILD/emergent_adaptive_test"

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
        printf 'SKIP  %-52s (mutation site not found -- test the harness)\n' "$name"
        failures=$((failures + 1))
        return
    fi
    if ! cmake --build "$BUILD" --target emergent_adaptive_test -j2 >/tmp/adaptive_build.log 2>&1; then
        printf 'CAUGHT %-52s (build failed)\n' "$name"
        return
    fi
    if "$TEST_BIN" >/tmp/adaptive_test.log 2>&1; then
        printf 'SURVIVED %-50s <<< the suite did NOT notice\n' "$name"
        failures=$((failures + 1))
    else
        printf 'CAUGHT %-52s (%s)\n' "$name" "$(grep -c '^  FAIL' /tmp/adaptive_test.log) assertions"
    fi
}

echo "adaptive visibility negative controls"
echo

# -- the correctness invariant ----------------------------------------------
# These are the important ones. Each removes a conservatism or a cache
# discipline, and each must be caught by the oracle comparison.

run_mutation "sphere test ignores the radius" \
    'if (dist < -s.radius) return false;' \
    'if (dist < 0.0f) return false;'

run_mutation "distance reject becomes non-conservative" \
    'if (d2 > reach * reach) return true;' \
    'if (d2 > reach * reach * 0.25f) return true;'

run_mutation "cone test fires without a validity gate" \
    'if (c.cone.valid() && !c.twoSided) {' \
    'if (true) {'

run_mutation "cone test ignores two-sided geometry" \
    'if (c.cone.valid() && !c.twoSided) {' \
    'if (c.cone.valid()) {'

run_mutation "REUSE returns the wrong cached value" \
    'visible = c.visible;
            ++metrics.reuses;' \
    'visible = true;
            ++metrics.reuses;'

run_mutation "invalidation no longer forces a recompute" \
    'if (c.invalidation != kInvNone) {
            decision = Decision::kFullRecompute;' \
    'if (false) {
            decision = Decision::kFullRecompute;'

run_mutation "a teleport no longer invalidates the world" \
    'if (motion_ == CameraMotion::kTeleport) {
        invalidateAll(kInvTeleport);' \
    'if (false) {
        invalidateAll(kInvTeleport);'

run_mutation "occlusion reuses a stale answer on a still camera" \
    'if (visible && occlusion_ != nullptr) {' \
    'if (visible && occlusion_ != nullptr && decision != Decision::kReuse) {'

# -- the geometry primitives -------------------------------------------------

run_mutation "normal cone accepts a cancelling normal average" \
    'if (sumLenSq < 0.25f) return Cone::invalid();' \
    'if (sumLenSq < -1.0f) return Cone::invalid();'

run_mutation "cone accepts a hemisphere-wide half angle" \
    'if (cone.cosAngle <= Cone::kMinUsableCos) return Cone::invalid();' \
    ''

run_mutation "frustum normalises its planes away" \
    'const float inv = 1.0f / std::sqrt(a * a + b * b + c * c);
        out.planes[index * 4 + 0] = a * inv;' \
    'const float inv = 1.0f;
        out.planes[index * 4 + 0] = a * inv;'

# -- LOD and the budget ------------------------------------------------------

run_mutation "LOD hysteresis is removed" \
    'if (prev > lod) {' \
    'if (false) {'

run_mutation "the budget may skip an invalidated cluster" \
    'if (decision == Decision::kFullRecompute && fullBudget > 0) --fullBudget;' \
    'if (c.invalidation != kInvNone) { /* deferred past the budget */ } else if (fullBudget > 0) --fullBudget;'

# -- counters ----------------------------------------------------------------
# A metric that silently stops counting is a lie told in the HUD.

run_mutation "reuses are not counted" \
    '++metrics.reuses;' \
    ''

run_mutation "reuse ratio is reported as a constant" \
    'return n > 0.0 ? static_cast<double>(reuses) / n : 0.0;' \
    'return 1.0;'

# -- harness self-test -------------------------------------------------------
# A mutation the suite is *supposed* to ignore. If this one is "caught", the
# suite is matching on text rather than on behaviour, and every other control
# above is suspect.

cp "$BACKUP" "$SRC"
if mutate 'const char *g_section = "";' 'const char *g_section = "";  // cosmetic'; then
    if cmake --build "$BUILD" --target emergent_adaptive_test -j2 >/dev/null 2>&1 && "$TEST_BIN" >/dev/null 2>&1; then
        printf 'OK     %-52s (cosmetic change, correctly ignored)\n' "no false positive on a cosmetic edit"
    else
        printf 'BROKEN %-52s <<< the suite rejects a harmless edit\n' "no false positive on a cosmetic edit"
        failures=$((failures + 1))
    fi
else
    printf 'SKIP   %-52s (mutation site not found -- test the harness)\n' "no false positive on a cosmetic edit"
    failures=$((failures + 1))
fi

cp "$BACKUP" "$SRC"
echo
if [ "$failures" -eq 0 ]; then
    echo "all negative controls behaved as expected"
else
    echo "$failures control(s) did not behave as expected"
fi
exit "$failures"
