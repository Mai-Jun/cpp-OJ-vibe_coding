// api.js — fetch 封装（同源请求自动携带 Cookie）。
// login/register 成功后由页面写入 localStorage 的 oj_session 作为登录态提示；
// Cookie 本身为 HttpOnly，服务端据此鉴权。

const SESSION_KEY = "oj_session";

function getSession() {
  try {
    return JSON.parse(localStorage.getItem(SESSION_KEY) || "null");
  } catch {
    return null;
  }
}

function setSession(info) {
  localStorage.setItem(SESSION_KEY, JSON.stringify(info));
}

function clearSession() {
  localStorage.removeItem(SESSION_KEY);
}

// 发送请求并解析 JSON；业务失败（HTTP 错误或 {ok:false}）时抛 Error(message)。
async function apiFetch(path, options = {}) {
  const opts = {
    method: options.method || "GET",
    headers: { "Content-Type": "application/json", ...(options.headers || {}) },
    signal: options.signal,
  };
  if (options.body) opts.body = JSON.stringify(options.body);

  const res = await fetch(path, opts);
  let data = null;
  try {
    data = await res.json();
  } catch {
    // 非 JSON 响应
  }
  if (!res.ok) {
    const msg = data && data.error ? data.error : `HTTP ${res.status}`;
    throw new Error(msg);
  }
  return data;
}

// HTML 转义（插入 innerHTML 前使用，防 XSS）。
function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, c => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  }[c]));
}

// 轻量 Markdown 渲染：仅处理代码块、行内代码、标题、粗体、链接、换行。
// 评测系统题目描述为管理员录入的简单 Markdown，足够展示。
function renderMarkdown(md) {
  const src = String(md == null ? "" : md);
  let html = "";
  let inCode = false;
  const codeBuf = [];
  const lines = src.replace(/\r\n/g, "\n").split("\n");
  const emit = (t) => { html += t; };

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const fence = line.match(/^\s*```/);
    if (fence) {
      if (!inCode) {
        inCode = true;
        codeBuf.length = 0;
        emit('<pre class="md-code"><code>');
      } else {
        inCode = false;
        emit(esc(codeBuf.join("\n")) + "</code></pre>\n");
      }
      continue;
    }
    if (inCode) {
      codeBuf.push(line);
      continue;
    }
    const trimmed = line.trim();
    if (!trimmed) { emit("<br>"); continue; }
    if (/^#{1,6}\s/.test(line)) {
      const level = line.match(/^(#{1,6})\s/)[1].length;
      const text = inlineFormat(line.replace(/^#{1,6}\s/, ""));
      emit(`<h${level} class="md-h">${text}</h${level}>`);
      continue;
    }
    if (/^(-{3,}|\*{3,})$/.test(trimmed)) { emit("<hr>"); continue; }
    emit(`<p>${inlineFormat(line)}</p>`);
  }
  if (inCode) emit(esc(codeBuf.join("\n")) + "</code></pre>");
  return html;
}

function inlineFormat(t) {
  let s = esc(t);
  s = s.replace(/`([^`]+)`/g, '<code class="md-inline">$1</code>');
  s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
  s = s.replace(/\*([^*]+)\*/g, "<em>$1</em>");
  s = s.replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>');
  return s;
}

// 简易相对时间（提交记录展示）。
function timeAgo(str) {
  if (!str) return "";
  const t = new Date(str.replace(" ", "T"));
  if (isNaN(t.getTime())) return esc(str);
  const diff = Math.max(0, (Date.now() - t.getTime()) / 1000);
  if (diff < 60) return "刚刚";
  if (diff < 3600) return `${Math.floor(diff / 60)} 分钟前`;
  if (diff < 86400) return `${Math.floor(diff / 3600)} 小时前`;
  return `${Math.floor(diff / 86400)} 天前`;
}

// 7 类结果 / 单用例状态 → 徽章样式类。
function statusClass(status) {
  const map = {
    "Accepted": "st-ac", "Wrong Answer": "st-wa", "Time Limit Exceeded": "st-tle",
    "Memory Limit Exceeded": "st-mle", "Runtime Error": "st-re", "Compile Error": "st-ce",
    "System Error": "st-se",
    "AC": "st-ac", "WA": "st-wa", "TLE": "st-tle", "MLE": "st-mle",
    "RE": "st-re", "SE": "st-se",
  };
  return map[status] || "st-pending";
}

var DIFF_CLASS = { easy: "badge easy", medium: "badge medium", hard: "badge hard" };

// 提交代码并返回 submission_id。
async function submitSolution(problemId, code) {
  const data = await apiFetch("/api/submissions", {
    method: "POST", body: { problem_id: problemId, code },
  });
  if (!data.ok) throw new Error(data.error || "提交失败");
  return data.submission_id;
}

// 轮询单个提交直至完成；intervalMs 间隔，maxMs 上限（超时抛错）。
async function pollSubmission(id, intervalMs = 1200, maxMs = 120000) {
  const start = Date.now();
  for (;;) {
    const data = await apiFetch(`/api/submissions/${id}`);
    if (!data.ok || !data.submission) throw new Error(data.error || "查询失败");
    if (data.submission.done) return data.submission;
    if (Date.now() - start > maxMs) throw new Error("评测超时，请稍后查看提交记录");
    await new Promise(r => setTimeout(r, intervalMs));
  }
}

// 加载我的提交记录（供提交记录页复用）。
async function loadMySubmissions(limit = 50) {
  const data = await apiFetch(`/api/submissions?limit=${limit}`);
  if (!data.ok) throw new Error(data.error || "加载失败");
  return data.submissions || [];
}