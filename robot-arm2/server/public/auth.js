// 全站共用的登录工具
//
// 各页面引入本文件后即可：
//   api(path, body)     统一的请求封装（非 2xx 时返回 {ok:false, error}）
//   getMe()             取当前登录账号，未登录返回 null
//   requireLogin()      页面守卫：未登录就跳转到登录页，并记住原地址
//   doLogout()          退出登录
//   renderUserBar(el)   在指定容器里渲染「昵称 + 退出」或「登录」按钮

/** 统一请求封装。返回 {ok:true, data} 或 {ok:false, error} */
async function api(path, body) {
  const opts = body
    ? {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }
    : {};
  try {
    const res = await fetch(path, opts);
    const data = await res.json().catch(() => ({}));
    if (!res.ok) return { ok: false, error: data.error || `请求失败（HTTP ${res.status}）`, status: res.status };
    return { ok: true, data };
  } catch (e) {
    return { ok: false, error: '服务器无响应', status: 0 };
  }
}

/** 当前登录账号，未登录返回 null */
async function getMe() {
  const r = await api('/api/me');
  return r.ok && r.data.authed ? r.data.account : null;
}

/**
 * 页面守卫：未登录则跳转到登录页，并带上 next 参数以便登录后跳回。
 * 已登录则返回账号对象。
 */
async function requireLogin() {
  const me = await getMe();
  if (!me) {
    const next = encodeURIComponent(location.pathname + location.search);
    location.replace('/login?next=' + next);
    return null;
  }
  return me;
}

async function doLogout() {
  await api('/api/logout', {});
  location.href = '/';
}

/**
 * 在页面上渲染登录状态。
 *   <div id="userbar"></div>
 *
 * 已登录显示「昵称 [管理员] [退出]」，未登录显示「[登录]」。
 */
function renderUserBar(el, account) {
  if (typeof el === 'string') el = document.getElementById(el);
  if (!el) return;

  if (!account) {
    el.innerHTML = '<a class="userbar-login" href="/login">登录</a>';
    return;
  }

  const badge = account.role === 'admin'
    ? '<span class="badge">管理员</span>'
    : '';

  el.innerHTML =
    '<span class="userbar-name">' + escapeHtml(account.nickname) + '</span>' +
    badge +
    '<button class="mini" onclick="doLogout()">退出</button>';
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
  ));
}
