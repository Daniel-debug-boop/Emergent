#!/usr/bin/env bash
# Do the LOD tests actually test anything?
#
# A suite that has never failed is not evidence. This breaks the code on
# purpose, one rule at a time, rebuilds, and requires the suite to notice. A
# mutation that survives means one of two things: the rule it targets is not
# load-bearing, or the test that is supposed to check it does not. Both are
# worth knowing before a renderer depends on the rule.
#
# Usage:  tools/verify/lod_controls.sh [build-dir]
#         EMERGENT_BUILD_DIR=build/native tools/verify/lod_controls.sh
#
# The source is restored on exit, including on interrupt or on a failed build.
set -uo pipefail

BUILD_DIR="${1:-${EMERGENT_BUILD_DIR:-build/native}}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TEST="$BUILD_DIR/emergent_lod_test"
SRC="$ROOT/native/src/lod.cpp"
HDR="$ROOT/native/include/emergent/lod.hpp"

if [ ! -x "$TEST" ]; then
  echo "error: $TEST not found or not executable." >&2
  echo "       Configure and build the engine first, or pass the build directory." >&2
  exit 2
fi

WORK="$(mktemp -d)"
CAUGHT=0
SURVIVED=0
SKIPPED=0
declare -a SURVIVORS=()

# Put the sources back. Deliberately does NOT remove $WORK: the loop below calls
# this between every mutation, and the first version of this script deleted the
# backup here, so every mutation after the first one reported "site not found"
# and the harness passed 21 skips off as a run.
restore() {
  [ -f "$WORK/lod.cpp" ] && cp "$WORK/lod.cpp" "$SRC"
  [ -f "$WORK/lod.hpp" ] && cp "$WORK/lod.hpp" "$HDR"
  return 0
}

cleanup() {
  restore
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

cp "$SRC" "$WORK/lod.cpp"
cp "$HDR" "$WORK/lod.hpp"

# run_mutation <label> <file:old:new> ...
# The file:old:new triples are applied with python so the mutation can be
# expressed as an exact string rather than a line number. Line numbers move
# every time anyone edits the file above the mutation, and a control harness
# that silently starts editing the wrong line is worse than no harness.
# Each remaining argument is one mutation site, "src:OLD@@@NEW" or
# "hdr:OLD@@@NEW". The separator is @@@ and not = or => because the old text is
# C++ and is full of "=" -- the first version of this script split on "=>",
# which does not occur in a single one of the sites, so every mutation was
# reported as "site not found" and the harness looked like it had run. Only the last one is used; the extra arguments exist so a
# site that spans two lines can be written as one expression.
run_mutation() {
  local label="$1"; shift
  local spec file old new

  restore
  cp "$WORK/lod.cpp" "$SRC"
  cp "$WORK/lod.hpp" "$HDR"

  file="$SRC"; old=""; new=""
  for spec in "$@"; do
    case "$spec" in
      src:*) file="$SRC"; spec="${spec#src:}" ;;
      hdr:*) file="$HDR"; spec="${spec#hdr:}" ;;
    esac
    old="${spec%%@@@*}"
    new="${spec#*@@@}"
  done

  if ! python3 - "$file" "$old" "$new" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
if old not in s:
    sys.stderr.write("mutation site not found\n")
    sys.exit(3)
open(path, "w").write(s.replace(old, new, 1))
PY
  then
    printf '  SKIP  %-52s (site not found)\n' "$label"
    SKIPPED=$((SKIPPED + 1))
    return
  fi

  if ! cmake --build "$BUILD_DIR" --target emergent_lod_test -j1 >/dev/null 2>&1; then
    printf '  SKIP  %-52s (does not compile)\n' "$label"
    SKIPPED=$((SKIPPED + 1))
    restore
    return
  fi

  if "$TEST" >/dev/null 2>&1; then
    printf '  LIVE  %-52s (suite still passes)\n' "$label"
    SURVIVED=$((SURVIVED + 1))
    SURVIVORS+=("$label")
  else
    printf '  CAUGHT %-51s\n' "$label"
    CAUGHT=$((CAUGHT + 1))
  fi
  restore
}

echo "LOD mutation controls"
echo "build: $BUILD_DIR"
echo

# -- the error budget -------------------------------------------------------
run_mutation "error budget ignored (target_error = 1.0)" \
  'src:const float relativeBudget = budget / errorScale;@@@const float relativeBudget = 1.0f;'

run_mutation "error not converted out of relative units" \
  'src:out.error = relativeError * errorScale;@@@out.error = relativeError;'

run_mutation "the reverse conversion dropped" \
  'src:const float relativeBudget = budget / errorScale;@@@const float relativeBudget = budget;'

# -- the reduction guarantee ------------------------------------------------
run_mutation "no-reduction guard removed" \
  'src:if (simplified.size() >= current.size()) {@@@if (false) {'

run_mutation "reduction guard compares against the wrong buffer" \
  'src:if (simplified.size() >= current.size()) {@@@if (simplified.size() >= simplified.size() * 2) {'

# EXPECTED TO SURVIVE -- redundant guards. Both the ratio check and the
# post-simplification "did it actually reduce" check refuse the same degenerate
# request, so removing either one is caught by the other. That is defence in
# depth rather than an untested rule, and it is listed here so the distinction
# is on the record instead of being inferred from a number.
run_mutation "a non-reducing ratio is accepted" \
  'src:if (ratio <= 0.0f || ratio >= 1.0f) {@@@if (ratio <= 0.0f) {'

# -- cluster correctness ---------------------------------------------------
run_mutation "cluster index offset multiplied by three" \
  'src:c.indexOffset = ml.triangle_offset;@@@c.indexOffset = ml.triangle_offset * 3;'

run_mutation "cluster index count multiplied by three" \
  'src:c.indexCount = ml.triangle_count * 3;@@@c.indexCount = ml.triangle_count * 9;'

# EXPECTED TO SURVIVE -- the two halves of the cone gate each catch the other's
# mutation. Removing the cutoff<1 test leaves the axis-length test, which
# rejects the sentinel because the sentinel never writes an axis. Removing the
# axis test leaves the cutoff test, which rejects it because the sentinel's
# cutoff is exactly 1. Neither can go without the other noticing, which is what
# redundancy is for. The control that removes BOTH is below.
run_mutation "degenerate-cone sentinel accepted" \
  'src:bounds.cone_cutoff > 0.0f && bounds.cone_cutoff < 1.0f &&@@@bounds.cone_cutoff > 0.0f &&'

run_mutation "zero-length cone axis accepted" \
  'src:                       axisLen2 > 0.5f;@@@                       true;'

# ...and this one removes the whole gate, which nothing else can catch, so it is
# the control that actually proves the sentinel assertion is load-bearing.
run_mutation "the entire cone-usability gate removed" \
  'src:        c.coneUsable = closed && bounds.cone_cutoff > 0.0f && bounds.cone_cutoff < 1.0f &&
                       axisLen2 > 0.5f;  // a unit axis is len2 == 1; 0.5 rejects the zero axis@@@        c.coneUsable = true;'

run_mutation "the closed-surface gate removed" \
  'src:c.twoSided = !closed;@@@c.twoSided = false;'

run_mutation "the closed-surface gate inverted" \
  'src:c.coneUsable = closed &&@@@c.coneUsable = !closed &&'

run_mutation "clusters skip their last triangle" \
  'src:        for (uint32_t t = 0; t < ml.triangle_count; ++t) {@@@        for (uint32_t t = 0; t + 1 < ml.triangle_count; ++t) {'

run_mutation "vertex range taken from the wrong end" \
  'src:                hi = std::max(hi, global);@@@                hi = std::min(hi, global);'

run_mutation "the explicit border lock is dropped" \
  'src:        if (options.lockBorder) {@@@        if (false) {'

run_mutation "the border mask ignores single-occurrence edges" \
  'src:        if (j - i == 1 && edges[i] != 0) {@@@        if (j - i == 0 && edges[i] != 0) {'

run_mutation "the border mask locks every vertex" \
  'src:                if (border[v]) {@@@                if (true) {'

# -- welding ---------------------------------------------------------------
# EXPECTED TO SURVIVE -- performance, not correctness. The hash only picks a
# bucket; equality is decided by VertexEqual, which compares bits. A worse hash
# costs time and cannot change the answer, so there is nothing for a
# correctness test to catch. It is here so that the reason it survives is
# written down rather than guessed at later.
run_mutation "the vertex hash is degenerate" \
  'src:    for (float f : v) h = lodMix(h, bitsOf(f));@@@    for (float f : v) h = lodMix(h, bitsOf(f) ^ 1u);'

run_mutation "welding is a no-op" \
  'src:            remap[v] = next;@@@            remap[v] = v; welded = 0;'

run_mutation "weld output order follows the hash table" \
  'src:            const uint32_t next = static_cast<uint32_t>(seen.size());
            seen.emplace(key, next);@@@            const uint32_t next = static_cast<uint32_t>(seen.size()) + v;
            seen.emplace(key, next);'

# -- deviation measurement -------------------------------------------------
run_mutation "deviation returns zero" \
  'src:        if (best > worst) worst = best;@@@        (void)best;'

run_mutation "deviation keeps the worst instead of the best" \
  'src:            if (best < 0.0f || d < best) best = d;@@@            if (best < 0.0f || d > best) best = d;'

# EXPECTED TO SURVIVE -- performance, not correctness. See the note on the hash.
run_mutation "the early-out short-circuits the search" \
  'src:            if (best == 0.0f) break;@@@            if (best == 1e30f) break;'

# -- level ordering --------------------------------------------------------
run_mutation "levels built from the source instead of the level above" \
  'src:        current = out.indices;@@@        current = chain.levels[0].indices;'

run_mutation "vertex cache optimization skipped" \
  'src:    meshopt_optimizeVertexCache(reordered.data(), current.data(), current.size(), vertexCount);@@@    reordered = current;'

echo
echo "caught $CAUGHT, survived $SURVIVED, skipped $SKIPPED"
if [ "$SURVIVED" -gt 0 ]; then
  echo
  echo "survivors, and what each one means:"
  for s in "${SURVIVORS[@]}"; do
    echo "  - $s"
  done
  echo
  echo "A survivor is not automatically a defect. Some of these are redundant"
  echo "guards where either of two checks catches the mutation, which is"
  echo "defence in depth rather than an untested rule. Which is which has to"
  echo "be decided by reading the two code paths, not by counting."
fi
[ "$SKIPPED" -eq 0 ] || echo "note: $SKIPPED site(s) skipped -- a skipped control is no control."
exit 0
