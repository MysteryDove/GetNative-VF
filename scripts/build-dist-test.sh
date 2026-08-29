#!/usr/bin/env bash

set -euo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
app_dir="${repo_root}/app"
tauri_dir="${app_dir}/src-tauri"
stage_dir="${tauri_dir}/bundle-stage"
target_dir="${tauri_dir}/target/release"
output_dir="${app_dir}/dist-test"

if [ "$(uname -s)" != "Linux" ]; then
  echo "build-dist-test.sh currently supports Linux only" >&2
  exit 1
fi

if ! command -v npm >/dev/null 2>&1; then
  echo "npm is required to build the test distribution" >&2
  exit 1
fi

if [ ! -x "${app_dir}/node_modules/.bin/tauri" ]; then
  npm --prefix "${app_dir}" ci
fi

# Test distributions exercise both GPU implementations by default. Callers
# can still request cpu, cuda, or vulkan explicitly through the existing knob.
export GETNATIVE_LINUX_BACKENDS="${GETNATIVE_LINUX_BACKENDS:-cuda-vulkan}"
echo "linux_backends=${GETNATIVE_LINUX_BACKENDS}"

# Build and stage the engine once. This command creates an artifact for testing;
# the regular build:engine command remains the CTest-gated release path.
npm --prefix "${app_dir}" run build:engine -- --skip-tests
tauri_config='{"build":{"beforeBuildCommand":"npm run build"}}'
npm --prefix "${app_dir}" run tauri build -- --no-bundle --ci --config "${tauri_config}"

gui_binary="${target_dir}/getnative-gui"
engine_binary="${stage_dir}/bin/getnative-engine"
provenance="${stage_dir}/build-provenance.json"

for required_file in "${gui_binary}" "${engine_binary}" "${provenance}"; do
  if [ ! -f "${required_file}" ]; then
    echo "build output is missing: ${required_file}" >&2
    exit 1
  fi
done

# Tauri resolves Linux resources from ../lib/<productName> relative to the
# real executable. Keep the executable in bin/ and provide a top-level link.
temporary_dir=$(mktemp -d "${app_dir}/.dist-test.XXXXXX")
trap 'rm -rf -- "${temporary_dir}"' EXIT
resource_dir="${temporary_dir}/lib/GetNative VF"

mkdir -p "${temporary_dir}/bin" "${resource_dir}"
chmod 0755 "${temporary_dir}"
install -m 0755 "${gui_binary}" "${temporary_dir}/bin/getnative-gui"
cp -a "${stage_dir}/." "${resource_dir}/"
ln -s "bin/getnative-gui" "${temporary_dir}/getnative-gui"

rm -rf -- "${output_dir}"
mv -- "${temporary_dir}" "${output_dir}"
trap - EXIT

echo "test_distribution=${output_dir}"
echo "run=${output_dir}/getnative-gui"
