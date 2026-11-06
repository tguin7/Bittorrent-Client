#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
make clean
make -j4 all
make test
printf '\nIntegration compilation and all automated tests passed.\n'
