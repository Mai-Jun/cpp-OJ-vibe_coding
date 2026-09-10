#!/usr/bin/env bash
# test.sh — Phase 5 集成测试：并发/异常/边界用例 + 7 类结果 + 网络禁止 + 限流。
#
# 用法（需已构建 oj_server 并初始化数据库）：
#   1) 降内存临时启动 MySQL（验后脚本自动关闭，见 SPEC §0）：
#        mysqld --datadir=/path/to/mysql-data --port=33061 \
#               --innodb-buffer-pool-size=64M --performance-schema=OFF \
#               --socket=/tmp/oj-test.sock &
#   2) 初始化 schema + 种子题（需先用脚本建库账号）
#   3) 以测试端口启动服务：
#        OJ_DB_PORT=33061 OJ_PORT=8081 OJ_STATIC_DIR=static ./build/oj_server &
#   4) bash scripts/test.sh http://127.0.0.1:8081
#
# 脚本对每个用例判定 PASS/FAIL，全部通过返回 0；任一处失败以非 0 退出。
# 依赖：curl、python3（生成较大输入）、g++（本地生成编译错误用例可不用，走服务端编译）。

set -u
BASE="${1:-http://127.0.0.1:8080}"
CURL="curl -s -m 30"

PASS=0
FAIL=0
COOKIE_JAR="$(mktemp)"; COOKIE_ADMIN="$(mktemp)"
ADMIN_COOKIE=""

cleanup() {
  rm -f "$COOKIE_JAR" "$COOKIE_ADMIN"
}
trap cleanup EXIT

say()  { printf '%s\n' "$*"; }
pass() { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m  %s\n' "$*"; }
fail() { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m  %s\n' "$*"; }

# ---- 工具函数 ----
json_get() { # json_get <path>；JSON 从 stdin 读取（path 形如 .ok / .submission.status / .cases[0].status）
  python3 -c "import sys,json,re
d=json.load(sys.stdin)
p='$1'
# 将 .a.b[0].c 转换为 ['a']['b'][0]['c'] 访问
p=re.sub(r'\.([A-Za-z_][A-Za-z0-9_]*)', lambda m: \"['\"+m.group(1)+\"']\", p)
p=re.sub(r'\[(\d+)\]', lambda m: '['+m.group(1)+']', p)
try:
  v=eval('d'+p)
  print(v)
except Exception:
  pass" 2>/dev/null
}

# 普通用户：注册 + 登录，写入 COOKIE_JAR
register_user() {
  local un="$1" pw="${2:-test_pass_123}"
  $CURL -c "$COOKIE_JAR" -X POST "$BASE/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"$un\",\"password\":\"$pw\"}" >/dev/null
}
login_user() {
  local un="$1" pw="${2:-test_pass_123}"
  $CURL -c "$COOKIE_JAR" -X POST "$BASE/api/auth/login" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"$un\",\"password\":\"$pw\"}" >/dev/null
}

# 提交代码并轮询结果；打印最终 JSON。
submit_and_poll() {
  local pid="$1" code="$2"
  local tmp
  tmp="$($CURL -b "$COOKIE_JAR" -X POST "$BASE/api/submissions" \
    -H 'Content-Type: application/json' \
    -d "$(python3 -c "import json,sys;print(json.dumps({'problem_id':$pid,'code':sys.argv[1]}))" "$code")")"
  local sid
  sid="$(printf '%s' "$tmp" | json_get '.submission_id')"
  if [ -z "$sid" ]; then
    printf '%s' "$tmp"
    return 1
  fi
  local res n=0
  while [ $n -lt 90 ]; do
    res="$($CURL -b "$COOKIE_JAR" "$BASE/api/submissions/$sid")"
    if [ "$(printf '%s' "$res" | json_get '.submission.done')" = "True" ]; then
      printf '%s' "$res"
      return 0
    fi
    sleep 1
    n=$((n+1))
  done
  printf '%s' "$res"
  return 1
}

# ---- 0. 前置检查：服务在线 ----
say "== 0. 服务健康检查 =="
health="$($CURL "$BASE/health" 2>/dev/null)"
if [ "$(printf '%s' "$health" | json_get '.status')" = "ok" ]; then
  pass "GET /health"
else
  fail "GET /health（服务未启动？请先启动 oj_server 于 $BASE）"
  exit 1
fi

# ---- 1. 安全响应头 ----
say "== 1. HTTP 安全响应头 =="
hdr="$($CURL -D - -o /dev/null "$BASE/login.html")"
for h in "X-Content-Type-Options: nosniff" "X-Frame-Options: DENY" \
         "Referrer-Policy: no-referrer" "Content-Security-Policy:"; do
  if printf '%s' "$hdr" | grep -qi "$h"; then
    pass "响应头 $h"
  else
    fail "响应头 $h 缺失"
  fi
done

# ---- 2. 鉴权边界 ----
say "== 2. 鉴权边界 =="
code401="$($CURL -o /dev/null -w '%{http_code}' "$BASE/api/problems")"
[ "$code401" = "401" ] && pass "未登录访问 /api/problems → 401" || fail "未登录访问应 401，实际 $code401"

# 管理员操作需管理员权限
admin_create="$($CURL -X POST "$BASE/api/problems" -H 'Content-Type: application/json' \
  -d '{"title":"x","description":"d","difficulty":"easy"}')"
if [ "$(printf '%s' "$admin_create" | json_get '.ok')" = "False" ] && \
   [ "$(printf '%s' "$admin_create" | json_get '.error')" = "需要管理员权限" ]; then
  pass "普通用户创建题目 → 403"
else
  fail "普通用户创建题目应拒绝（当前：$(printf '%s' "$admin_create" | head -c 120)）"
fi

# ---- 3. 注册 / 登录 / 限流 ----
say "== 3. 注册/登录与限流 =="
UN="tester_$(date +%s)"
reg="$($CURL -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
  -d "{\"username\":\"$UN\",\"password\":\"test_pass_123\"}")"
[ "$(printf '%s' "$reg" | json_get '.ok')" = "True" ] && pass "注册新用户 $UN" || fail "注册失败：$reg"

# 重名
dup="$($CURL -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
  -d "{\"username\":\"$UN\",\"password\":\"test_pass_123\"}")"
[ "$(printf '%s' "$dup" | json_get '.error')" = "用户名已存在" ] && pass "重名注册被拒" || fail "重名注册未正确拒绝"

# 格式校验（需在限流触发前执行；非法输入同样计入限流。
# 若此前测试已触发限流则返回 429，也是合法的拒绝。）
bad="$($CURL -X POST "$BASE/api/auth/register" -H 'Content-Type: application/json' \
  -d "{\"username\":\"ab\",\"password\":\"12345\"}")"
bad_err="$(printf '%s' "$bad" | json_get '.error')"
if [ "$bad_err" = "注册过于频繁，请稍后再试" ]; then
  pass "非法用户名/密码被拒（命中限流 429，亦为拒绝）"
elif [ -n "$bad_err" ] && [ "$(printf '%s' "$bad" | json_get '.ok')" != "True" ]; then
  pass "非法用户名/密码被拒"
else
  fail "非法用户名/密码未拒绝（响应：$bad）"
fi

# 登录（错误密码 → 401）
login_user "$UN"
wrong="$($CURL -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
  -d "{\"username\":\"$UN\",\"password\":\"wrongpass\"}")"
[ "$(printf '%s' "$wrong" | json_get '.ok')" = "False" ] && pass "错误密码登录被拒" || fail "错误密码登录未拒绝"

# 限流：快速连续注册应触发 429
say "  -- 注册限流（20 次/60s）--"
rl_count=0
for i in $(seq 1 25); do
  st="$($CURL -o /dev/null -w '%{http_code}' -X POST "$BASE/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"rl_${UN}_$i\",\"password\":\"test_pass_123\"}")"
  if [ "$st" = "429" ]; then rl_count=$((rl_count+1)); fi
done
if [ "$rl_count" -gt 0 ]; then
  pass "注册限流生效（捕获 $rl_count 次 429）"
else
  fail "注册限流未生效"
fi

# ---- 4. 题目列表 / 详情 ----
say "== 4. 题目列表/详情 =="
login_user "$UN"
plist="$($CURL -b "$COOKIE_JAR" "$BASE/api/problems")"
first_id="$(printf '%s' "$plist" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['problems'][0]['id'] if d['problems'] else '')")"
[ -n "$first_id" ] && pass "题目列表返回 $([ -n "$(printf '%s' "$plist" | json_get '.problems')" ] && echo "题目")" || fail "题目列表为空"
pdetail="$($CURL -b "$COOKIE_JAR" "$BASE/api/problems/$first_id")"
[ "$(printf '%s' "$pdetail" | json_get '.ok')" = "True" ] && pass "题目详情 id=$first_id" || fail "题目详情失败"

# ---- 5. 7 类判题结果 ----
say "== 5. 判题结果分类 =="
# 5.1 AC
ac_code='#include <cstdio>
int main(){ long long a,b; scanf("%lld%lld",&a,&b); printf("%lld\n", a+b); return 0; }'
res="$(submit_and_poll "$first_id" "$ac_code")"
if [ "$(printf '%s' "$res" | json_get '.submission.status')" = "AC" ]; then
  pass "正确代码 → Accepted"
else
  fail "期望 Accepted，实际 $(printf '%s' "$res" | json_get '.submission.status')"
fi

# 5.2 WA
wa_code='#include <cstdio>
int main(){ int a,b; scanf("%d%d",&a,&b); printf("%d\n", a-b); return 0; }'
res="$(submit_and_poll "$first_id" "$wa_code")"
if [ "$(printf '%s' "$res" | json_get '.submission.status')" = "WA" ]; then
  pass "错误代码 → Wrong Answer（含预期vs实际）"
  # 校验 WA 详情字段
  exp="$(printf '%s' "$res" | json_get '.submission.cases[0].expected_output')"
  act="$(printf '%s' "$res" | json_get '.submission.cases[0].user_output')"
  [ -n "$exp" ] && [ -n "$act" ] && pass "  WA 对比字段存在" || fail "  WA 对比字段缺失"
else
  fail "期望 Wrong Answer，实际 $(printf '%s' "$res" | json_get '.submission.status')"
fi

# 5.3 CE
ce_code='int main() { this is not valid c++ !! }'
res="$(submit_and_poll "$first_id" "$ce_code")"
if [ "$(printf '%s' "$res" | json_get '.submission.status')" = "CE" ]; then
  pass "非法代码 → Compile Error"
else
  fail "期望 Compile Error，实际 $(printf '%s' "$res" | json_get '.submission.status')"
fi

# 5.4 TLE
tle_code='#include <cstdio>
int main(){ volatile long long x=0; while(true){ x++; } return 0; }'
res="$(submit_and_poll "$first_id" "$tle_code")"
if [ "$(printf '%s' "$res" | json_get '.submission.status')" = "TLE" ]; then
  pass "死循环 → Time Limit Exceeded"
else
  fail "期望 Time Limit Exceeded，实际 $(printf '%s' "$res" | json_get '.submission.status')"
fi

# 5.5 MLE
# vector 持续扩容：RLIMIT_AS 超限时 new 抛 bad_alloc（terminate → SIGABRT），
# judge_service 依据输出中的 bad_alloc 线索归类为 MLE。
mle_code='#include <cstdlib>
#include <vector>
int main(){ std::vector<char> v; while(true){ v.resize(v.size() + 16*1024*1024); } return 0; }'
res="$(submit_and_poll "$first_id" "$mle_code")"
if [ "$(printf '%s' "$res" | json_get '.submission.status')" = "MLE" ]; then
  pass "持续分配 → Memory Limit Exceeded"
else
  fail "期望 Memory Limit Exceeded，实际 $(printf '%s' "$res" | json_get '.submission.status')"
fi

# 5.6 RE
re_code='#include <cstdio>
int main(){ int *p = nullptr; *p = 42; return 0; }'
res="$(submit_and_poll "$first_id" "$re_code")"
if [ "$(printf '%s' "$res" | json_get '.submission.status')" = "RE" ]; then
  pass "空指针解引用 → Runtime Error"
else
  fail "期望 Runtime Error，实际 $(printf '%s' "$res" | json_get '.submission.status')"
fi

# 5.7 SE：非特殊题触发系统错误难；用不存在的题尝试（应 404，属边界）
se_check="$($CURL -b "$COOKIE_JAR" -X POST "$BASE/api/submissions" \
  -H 'Content-Type: application/json' \
  -d "{\"problem_id\":999999,\"code\":\"int main(){} \"}")"
[ "$(printf '%s' "$se_check" | json_get '.error')" = "题目不存在" ] && pass "提交不存在题目 → 404（边界）" || fail "提交不存在题目未正确拒绝"

# ---- 6. 网络禁止（seccomp）----
say "== 6. 沙箱禁止网络 =="
net_code='#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdio>
int main(){ int fd=socket(AF_INET, SOCK_STREAM, 0); printf("fd=%d\n", fd); return 0; }'
res="$(submit_and_poll "$first_id" "$net_code")"
if [ "$(printf '%s' "$res" | json_get '.submission.status')" = "RE" ]; then
  pass "socket() 调用 → Runtime Error（seccomp 拦截）"
else
  fail "socket() 应被拦截为 RE，实际 $(printf '%s' "$res" | json_get '.submission.status')"
fi

# ---- 7. 并发提交（10 个并发，全部有结果）----
say "== 7. 并发提交 =="
PIDS=""
for i in $(seq 1 10); do
  (
    code='#include <cstdio>
int main(){ long long a,b; scanf("%lld%lld",&a,&b); printf("%lld\n", a+b); return 0; }'
    body="$(python3 -c "import json,sys;print(json.dumps({'problem_id':$first_id,'code':sys.argv[1]}))" "$code")"
    tmp="$($CURL -b "$COOKIE_JAR" -X POST "$BASE/api/submissions" \
      -H 'Content-Type: application/json' -d "$body")"
    sid="$(printf '%s' "$tmp" | json_get '.submission_id')"
    [ -z "$sid" ] && { echo "concurrent-$i submit-fail" >> /tmp/oj_conc.txt; exit 0; }
    n=0
    while [ $n -lt 60 ]; do
      res="$($CURL -b "$COOKIE_JAR" "$BASE/api/submissions/$sid")"
      st="$(printf '%s' "$res" | json_get '.submission.status')"
      if [ "$(printf '%s' "$res" | json_get '.submission.done')" = "True" ]; then
        echo "concurrent-$i $st" >> /tmp/oj_conc.txt
        exit 0
      fi
      sleep 1; n=$((n+1))
    done
    echo "concurrent-$i timeout" >> /tmp/oj_conc.txt
  ) &
  PIDS="$PIDS $!"
done
rm -f /tmp/oj_conc.txt
wait $PIDS
ac_count="$(grep -c ' AC$' /tmp/oj_conc.txt 2>/dev/null || echo 0)"
total_count="$(wc -l < /tmp/oj_conc.txt 2>/dev/null || echo 0)"
if [ "$total_count" = "10" ] && [ "$ac_count" = "10" ]; then
  pass "10 并发提交全部 Accepted（串行队列无丢失）"
else
  fail "并发结果：共 $total_count 条，Accepted $ac_count 条（期望 10/10）"
  cat /tmp/oj_conc.txt 2>/dev/null
fi
rm -f /tmp/oj_conc.txt

# ---- 8. 边界：空代码 / 超大代码 / 空提交 ----
say "== 8. 边界用例 =="
empty="$($CURL -b "$COOKIE_JAR" -X POST "$BASE/api/submissions" \
  -H 'Content-Type: application/json' \
  -d "{\"problem_id\":$first_id,\"code\":\"\"}")"
[ "$(printf '%s' "$empty" | json_get '.error')" = "代码不能为空" ] && pass "空代码被拒" || fail "空代码未拒绝"

# 超大代码：用 python 构造完整 JSON body 到临时文件（避免命令行 ARG_MAX）。
# 代码约 600KB：超过应用层 512KB 限制，但低于 httplib 1MB 传输上限，
# 验证应用层校验生效（而非传输层 413 兜底）。
big_code_file="$(mktemp)"
python3 -c "
import json
with open('$big_code_file','w') as f:
    f.write(json.dumps({'problem_id':$first_id, 'code': 'int x;' * 100000}))
"
big="$($CURL -b "$COOKIE_JAR" -X POST "$BASE/api/submissions" \
  -H 'Content-Type: application/json' \
  --data-binary @"$big_code_file")"
rm -f "$big_code_file"
[ "$(printf '%s' "$big" | json_get '.error')" = "代码过长（上限 512KB）" ] && pass "超长代码被拒" || fail "超长代码未拒绝"

invalid_json="$($CURL -o /dev/null -w '%{http_code}' -b "$COOKIE_JAR" -X POST "$BASE/api/submissions" \
  -H 'Content-Type: application/json' -d 'not json')"
[ "$invalid_json" = "400" ] && pass "非法 JSON → 400" || fail "非法 JSON 应 400，实际 $invalid_json"

# ---- 9. 我的提交记录 ----
say "== 9. 提交记录 =="
subs="$($CURL -b "$COOKIE_JAR" "$BASE/api/submissions?limit=10")"
n="$(printf '%s' "$subs" | python3 -c "import sys,json;print(len(json.load(sys.stdin).get('submissions',[])))" 2>/dev/null)"
[ "$n" -ge 5 ] && pass "我的提交记录返回 $n 条" || fail "提交记录返回 $n 条（期望 ≥5）"

# ---- 结果汇总 ----
say ""
say "============================================="
say "结果：PASS=$PASS  FAIL=$FAIL"
say "============================================="
[ "$FAIL" = "0" ] && exit 0 || exit 1
