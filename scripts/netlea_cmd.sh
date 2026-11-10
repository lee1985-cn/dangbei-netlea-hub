#!/bin/sh
# 尼特利滤桶命令包装：最多重试 4 次，成功返回 0
# 用法: netlea_cmd.sh "<url>"
url="$1"
i=1
r=""
while [ "$i" -le 4 ]; do
  r=$(curl -s -m 20 "$url")
  case "$r" in
    *'"sent":true'*) echo "$r"; exit 0 ;;
    *'"len":'*)      echo "$r"; exit 0 ;;
  esac
  i=$((i+1))
  sleep 2
done
echo "FAILED: $r"
exit 1
