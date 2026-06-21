#!/usr/bin/env bash
# Cross-build Nimbus for MLP1 inside the mlp1-toolchain container, with the UMRK
# workspace mounted so Catastrophe resolves as a sibling.
set -euo pipefail
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="$(cd "$APP_DIR/.." && pwd)"   # siblings: Nimbus, Catastrophe
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"
echo "=== Building Nimbus for MLP1 (workspace: $WORKSPACE) ==="
docker run --rm -v "$WORKSPACE":/workspace -w /workspace/Nimbus "$IMAGE" \
	make -C ports/mlp1
