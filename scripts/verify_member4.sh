#!/usr/bin/env bash
# Clean-build verification for Ubuntu / WSL2. Does not need a production tracker.
set -euo pipefail
cd "$(dirname "$0")/.."
make clean
make all
make test
echo "Member 4 infrastructure and existing peer-client tests passed."
