// auth.js — 登录态检查 / 登出 / 导航渲染（Phase 4）。
// 依赖 api.js（getSession / clearSession / apiFetch）。

// 检查登录态，可选地要求管理员身份；未登录跳转登录页，返回是否通过。
function requireLogin(needAdmin = false) {
  const s = getSession();
  if (!s) {
    location.href = "/login.html";
    return false;
  }
  if (needAdmin && s.role !== "admin") {
    location.href = "/problems.html";
    return false;
  }
  return true;
}

// 获取当前登录用户信息（localStorage 缓存，可能为空）。
function currentUser() {
  return getSession();
}

function isLoggedIn() {
  const s = getSession();
  return !!(s && s.username);
}

// 渲染顶部导航栏（在页面中 <nav class="topnav" id="topnav"></nav> 位置）。
function renderNav() {
  const nav = document.getElementById("topnav");
  if (!nav) return;
  const s = getSession();
  const user = s && s.username ? s.username : "";
  const isAdmin = s && s.role === "admin";

  let inner = '<span class="brand">OJ</span>';
  inner += '<div class="nav-links">';
  inner += '<a href="/problems.html">题目</a>';
  if (isLoggedIn()) {
    inner += '<a href="/submissions.html">提交记录</a>';
    if (isAdmin) inner += '<a href="/admin.html">管理后台</a>';
  }
  inner += "</div>";

  if (isLoggedIn()) {
    const initial = esc(user.charAt(0));
    inner += `<div class="nav-user-menu">` +
      `<button id="navUserBtn" class="nav-user-btn" aria-haspopup="true" aria-expanded="false">` +
      `<span class="avatar">${initial}</span>` +
      `<span class="uname">${esc(user)}</span>` +
      (isAdmin ? '<span class="badge tag">管理员</span>' : "") +
      '<span class="caret">▼</span></button>' +
      `<div class="user-dropdown" id="navUserMenu">` +
      `<div class="dd-head"><div class="dd-name">${esc(user)}</div>` +
      `<div class="dd-role">${isAdmin ? "管理员账号" : "普通用户"}</div></div>` +
      '<a href="/profile.html">个人中心</a>' +
      '<a href="/submissions.html">我的提交记录</a>' +
      (isAdmin ? '<a href="/admin.html">管理后台</a>' : "") +
      '<button id="navLogout" class="dd-logout">登出</button>' +
      '</div></div>';
  } else {
    inner += '<div class="nav-user"><a class="btn-sm btn-outline" href="/login.html">登录</a>' +
             '<a class="btn-sm" href="/register.html">注册</a></div>';
  }
  nav.innerHTML = inner;

  // 用户下拉菜单交互：点击切换、点击外部/Escape 关闭
  const userBtn = document.getElementById("navUserBtn");
  const userMenu = document.getElementById("navUserMenu");
  if (userBtn && userMenu) {
    userBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      const open = userMenu.classList.toggle("open");
      userBtn.setAttribute("aria-expanded", open ? "true" : "false");
    });
    document.addEventListener("click", (e) => {
      if (!userMenu.contains(e.target) && e.target !== userBtn) {
        userMenu.classList.remove("open");
        userBtn.setAttribute("aria-expanded", "false");
      }
    });
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape") {
        userMenu.classList.remove("open");
        userBtn.setAttribute("aria-expanded", "false");
      }
    });
  }

  const logoutBtn = document.getElementById("navLogout");
  if (logoutBtn) {
    logoutBtn.addEventListener("click", async () => {
      try { await apiFetch("/api/auth/logout", { method: "POST", body: {} }); } catch (e) {}
      clearSession();
      location.href = "/index.html";
    });
  }
}

// 页面加载完成后统一调用：渲染导航。
if (typeof document !== "undefined") {
  document.addEventListener("DOMContentLoaded", renderNav);
}
