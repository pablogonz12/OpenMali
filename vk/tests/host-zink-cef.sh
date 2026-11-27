#!/usr/bin/env bash
# Host-only diagnostic: fake backend never produces trustworthy pixels.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
icd=${1:-"$root/vk/out/native/mali_icd.json"}
chrome=${CHROME:-"$HOME/.cache/mali-orchestrator/host-dumps/cef-repro/download/chrome-linux64/chrome"}
out=${OUT:-"$HOME/.cache/mali-orchestrator/host-dumps/cef-repro"}
mkdir -p "$out"
[[ -f $icd && -x $chrome ]] || { echo "usage: CHROME=/path/to/chrome $0 /path/to/mali_icd.json" >&2; exit 2; }
cc "$root/vk/tests/host-zink-msaa.c" -o "$out/host-zink-msaa" -lEGL -lGLESv2 || exit 2
cat > "$out/steam-ui.html" <<'HTML'
<!doctype html><meta charset="utf-8"><style>
body {background:#142435;color:white;font:20px sans-serif}
header {position:fixed;inset:0 0 auto;background:#89a7;backdrop-filter:blur(12px);padding:20px}
main {display:flex;gap:20px;margin-top:100px;transform:perspective(800px) rotateY(-4deg)}
article {width:250px;height:300px;border-radius:30px;overflow:hidden;background:linear-gradient(30deg,#205,#5a8);box-shadow:0 0 20px #8af}
canvas,video {width:100%;border-radius:45px;filter:blur(1px)}
</style><header>STORE　 LIBRARY　 COMMUNITY</header><main>
<article><canvas id="c" width="250" height="200"></canvas>Recent games</article>
<article><video poster="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='250' height='200'%3E%3Crect width='250' height='200' fill='%23359'/%3E%3C/svg%3E"></video>Play now</article>
</main><script>let x=c.getContext('2d');for(let i=0;i<100;i++){x.fillStyle=`hsl(${i*9},80%,55%)`;x.beginPath();x.arc(i*19%250,i*31%200,20,0,7);x.fill()}</script>
HTML
for arch in v13 v11; do
  export VK_ICD_FILENAMES="$icd" MALI_BACKEND=fake MALI_FAKE_GPU="$arch"
  export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink EGL_PLATFORM=surfaceless
  timeout 120 eglinfo -B >"$out/eglinfo-$arch.log" 2>&1
  timeout 120 "$out/host-zink-msaa" >"$out/msaa-$arch.log" 2>&1
  echo "$arch msaa exit=$?"
  for target in ui gpu; do
    url="file://$out/steam-ui.html"; [[ $target == gpu ]] && url=chrome://gpu
    timeout 120 "$chrome" --headless=new --ozone-platform=headless --use-gl=angle \
      --use-angle=gl-egl --enable-gpu-rasterization --ignore-gpu-blocklist \
      --enable-logging=stderr --v=1 --no-sandbox --disable-dev-shm-usage \
      --user-data-dir="$out/profile-$arch-$target" --dump-dom "$url" \
      >"$out/$target-$arch.dom" 2>"$out/$target-$arch.log"
    echo "$arch $target exit=$?"
  done
  grep -hE 'mali_cmd: unsupported|vkEndCommandBuffer failed|GUILTY_CONTEXT_RESET|zink:' \
    "$out/msaa-$arch.log" "$out/ui-$arch.log" "$out/gpu-$arch.log" | sort -u || true
done
