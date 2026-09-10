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