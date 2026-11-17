#!/bin/bash
# SPDX-License-Identifier: MIT
# R4 batch feature probe: feat.sh <name> <egrep regex over case names> [every-Nth=1] [limit=300]
# Builds lists/feat-<name>.txt from the pinned mustpass and runs it on the Poco with our ICD (results: runs/qdeqp/poco-ours-feat-<name>.*).
export PATH=$PATH:$HOME/Android/Sdk/platform-tools
M=$(realpath $(dirname "$0"))/../../third_party/cts-src/external/vulkancts/mustpass/main/vk-default
n=$1; re=$2; every=${3:-1}; lim=${4:-300}
L=$(realpath $(dirname "$0"))/lists/feat-$n.txt
cat $M/*.txt $M/pipeline/monolithic.txt $M/image/*.txt 2>/dev/null | grep -E "$re" | grep -v "object_management.\(alloc_callback\|max_\|multithreaded\)" | awk -v e=$every 'NR%e==1||e==1' | head -$lim > $L
echo "$(wc -l <$L) cases"
rm -f ~/Projects/mali-gaming/runs/qdeqp/poco-ours-feat-$n.*
cd ~/Projects/mali-gaming; timeout 240 scripts/quick/qdeqp poco $L --icd ours --chunk 300 --case-timeout 45 --keep 2>&1 | tail -1
