#!/bin/bash
# Package the staged native libraries and resources into an APK.
# usage: build_apk.sh [debug|release]   (default debug)
#   LIBERTY_GPU=mali build_apk.sh   builds the Mali variant (system Vulkan
#   driver by default, version name suffixed "-mali"); default is adreno.
set -euo pipefail
. "$(dirname "$0")/env.sh"
VARIANT="${1:-debug}"
TASK="assemble$(tr '[:lower:]' '[:upper:]' <<< "${VARIANT:0:1}")${VARIANT:1}"

[ -f "$JNILIBS/libmain.so" ] || { echo "no libmain.so staged - run build_native.sh"; exit 1; }

cat > "$APP_DIR/local.properties" <<EOF
sdk.dir=$(cygpath -m "$ANDROID_HOME" 2>/dev/null || echo "$ANDROID_HOME")
EOF

cd "$APP_DIR"
./gradlew --no-daemon "$TASK" "-PlibertyGpu=${LIBERTY_GPU:-adreno}"
ls -la "$APP_DIR/app/build/outputs/apk/$VARIANT/"
