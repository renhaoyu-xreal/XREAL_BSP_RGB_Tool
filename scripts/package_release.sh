#!/bin/bash
# ----------------------------------------------------------------------------
# RecordLabC - Release package builder
#
# Produces dist/RecordLabC, a movable folder containing:
#   - build/recordlabc and helper executables
#   - run.sh / RecordLabC.sh launchers
#   - install_dependencies.sh for first-time machines
#   - config, scripts, subnodes, third_party runtime assets
# ----------------------------------------------------------------------------

set -euo pipefail

PROJECT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
BUILD_DIR="${PROJECT_DIR}/build"
DIST_ROOT="${PROJECT_DIR}/dist"
PACKAGE_DIR="${DIST_ROOT}/RecordLabC"
ARCHIVE_PATH="${DIST_ROOT}/RecordLabC-linux-x86_64.tar.gz"

SKIP_BUILD=0
CLEAN_DIST=1

usage() {
  cat <<'EOF'
Usage: scripts/package_release.sh [options]

Options:
  --skip-build   Reuse existing build/ outputs
  --keep-dist    Do not delete dist/RecordLabC before packaging
  -h, --help     Show this help
EOF
}

while (($# > 0)); do
  case "$1" in
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --keep-dist)
      CLEAN_DIST=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

require_file() {
  local path="$1"
  if [[ ! -e "${path}" ]]; then
    echo "Missing required package input: ${path}" >&2
    exit 1
  fi
}

copy_path() {
  local src="$1"
  local dst="$2"

  require_file "${PROJECT_DIR}/${src}"
  mkdir -p "$(dirname "${PACKAGE_DIR}/${dst}")"
  rsync -a --delete \
    --exclude "__pycache__/" \
    --exclude "*.pyc" \
    --exclude ".git/" \
    "${PROJECT_DIR}/${src}" "${PACKAGE_DIR}/${dst}"
}

copy_executable() {
  local name="$1"

  require_file "${BUILD_DIR}/${name}"
  install -Dm755 "${BUILD_DIR}/${name}" "${PACKAGE_DIR}/build/${name}"
}

if [[ "${SKIP_BUILD}" != "1" ]]; then
  "${PROJECT_DIR}/build.sh"
fi

if ! command -v rsync >/dev/null 2>&1; then
  echo "rsync is required for packaging. Install it with: sudo apt-get install -y rsync" >&2
  exit 1
fi

if [[ "${CLEAN_DIST}" == "1" ]]; then
  rm -rf "${PACKAGE_DIR}"
fi
mkdir -p "${PACKAGE_DIR}/build" "${DIST_ROOT}"

copy_executable "recordlabc"
copy_executable "bsp_main_subnode"
copy_executable "helen_main_subnode"
copy_executable "imu_sim_main_subnode"
copy_executable "nviz_node_subnode"
copy_executable "recordlabc_doctor"
copy_executable "recordlabc_agent_probe"
copy_executable "recordlabc_agent_cmd"

copy_path "config/" "config/"
copy_path "scripts/" "scripts/"
copy_path "subnodes/" "subnodes/"
copy_path "third_party/" "third_party/"
copy_path "docs/" "docs/"
copy_path "README.md" "README.md"
copy_path "RecordLabC技术文档.md" "RecordLabC技术文档.md"
copy_path "run.sh" "run.sh"
copy_path "start.sh" "start.sh"
copy_path "doctor.sh" "doctor.sh"
copy_path "setup.sh" "setup.sh"
copy_path "install_dependencies.sh" "install_dependencies.sh"
copy_path "setup_xreal_runtime.sh" "setup_xreal_runtime.sh"

cat > "${PACKAGE_DIR}/RecordLabC.sh" <<'EOF'
#!/bin/bash
set -euo pipefail

APP_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
exec "${APP_DIR}/run.sh" "$@"
EOF

cat > "${PACKAGE_DIR}/create_desktop_launcher.sh" <<'EOF'
#!/bin/bash
set -euo pipefail

APP_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DESKTOP_DIR="${HOME}/Desktop"
LAUNCHER_PATH="${DESKTOP_DIR}/RecordLabC.desktop"

mkdir -p "${DESKTOP_DIR}"
cat > "${LAUNCHER_PATH}" <<DESKTOP
[Desktop Entry]
Type=Application
Name=RecordLabC
Comment=Launch RecordLabC
Exec=${APP_DIR}/RecordLabC.sh
Icon=utilities-terminal
Terminal=true
Categories=Development;Science;
DESKTOP

chmod +x "${LAUNCHER_PATH}"
echo "Desktop launcher created: ${LAUNCHER_PATH}"
EOF

cat > "${PACKAGE_DIR}/PACKAGE_README.txt" <<'EOF'
RecordLabC packaged build

First-time setup on a new Ubuntu machine:
  tar -xzf RecordLabC-linux-x86_64.tar.gz
  cd RecordLabC
  ./install_dependencies.sh
  ./RecordLabC.sh

install_dependencies.sh runs setup.sh first, then setup_xreal_runtime.sh.
The bundled xreal_glasses-0.4.3-py3-none-any.whl is installed into:
  runtime/xreal_runtime/site-packages/

To skip XREAL runtime setup when only launching the GUI:
  RECORDLABC_SKIP_XREAL_RUNTIME=1 ./install_dependencies.sh

Optional:
  Run create_desktop_launcher.sh to place a launcher on Desktop.

If the desktop environment opens .sh files in an editor, right-click the file,
enable "Allow executing file as program", then choose "Run in Terminal".
EOF

find "${PACKAGE_DIR}" -type f \( -name "*.sh" -o -path "*/subnodes/*/shell/*" \) -exec chmod +x {} \;
chmod +x "${PACKAGE_DIR}/RecordLabC.sh" "${PACKAGE_DIR}/create_desktop_launcher.sh"

tar -C "${DIST_ROOT}" -czf "${ARCHIVE_PATH}" "RecordLabC"

echo "=================================================="
echo "RecordLabC package is ready:"
echo "  Folder : ${PACKAGE_DIR}"
echo "  Archive: ${ARCHIVE_PATH}"
echo ""
echo "User entry points:"
echo "  ${PACKAGE_DIR}/install_dependencies.sh"
echo "  ${PACKAGE_DIR}/RecordLabC.sh"
echo "=================================================="
