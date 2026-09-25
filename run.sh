#!/usr/bin/env bash
# Launch the EMERGENT game locally.
# The project ships its own dependency-free static server so the runtime is
# identical to the one the preview and production deploy use.
set -e
cd "$(dirname "$0")"
exec node tools/serve.mjs
