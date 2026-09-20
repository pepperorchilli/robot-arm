// 留言板前端逻辑

let authed = false;   // 是否已登录（管理员身份）

function $(id) { return document.getElementById(id); }

function fmt(t) {
  const d = new Date(t);
  const p = n => (n < 10 ? '0' : '') + n;
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate())
    + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
}

// 转义 HTML，防止留言里写 <script> 被当代码执行（XSS）
function esc(s) {
  return String(s).replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
  ));
}

// ---------------- 登录状态 ----------------
//
// 登录一次即为「管理员」，之后回复/删除都不再需要输密码。
// 这和机械臂控制台共用同一个密码和会话。

async function checkAuth() {
  try {
    const res = await fetch('/api/auth');
    authed = (await res.json()).authed === true;
  } catch {
    authed = false;
  }
  renderAuthBar();
}

function renderAuthBar() {
  const bar = $('authbar');
  if (!bar) return;
  if (authed) {
    bar.innerHTML = '<span class="badge admin">管理员</span>'
      + '<button class="mini" onclick="doLogout()">退出</button>';
  } else {
    bar.innerHTML = '<button class="mini" onclick="doLogin()">管理员登录</button>';
  }
}

async function doLogin() {
  const password = prompt('管理员密码：');
  if (!password) return;
  try {
    const res = await fetch('/api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ password }),
    });
    if (!res.ok) {
      const e = await res.json().catch(() => ({}));
      alert(e.error || '登录失败');
      return;
    }
    authed = true;
    renderAuthBar();
    load();          // 重新渲染，让管理按钮出现
  } catch {
    alert('服务器无响应');
  }
}

async function doLogout() {
  try { await fetch('/api/logout', { method: 'POST' }); } catch { /* 忽略 */ }
  authed = false;
  renderAuthBar();
  load();
}

// ---------------- 留言 ----------------

async function load() {
  const res = await fetch('/api/messages');
  const list = await res.json();
  const box = $('list');
  if (!list.length) {
    box.innerHTML = '<div class="subtitle">还没有留言，来抢沙发～</div>';
    return;
  }
  box.innerHTML = list.map(m => {
    let replies = '';
    if (m.replies && m.replies.length) {
      replies = '<div class="reply-list">' + m.replies.map(r =>
        '<div class="reply"><span class="rwho">博主：</span>' + esc(r.content)
        + '<span class="when"> ' + fmt(r.time) + '</span></div>'
      ).join('') + '</div>';
    }

    // 管理按钮只在登录后出现
    const actions = authed
      ? '<div class="actions">'
        + '<button onclick="replyBox(' + m.id + ')">回复</button>'
        + '<button class="del" onclick="delBox(' + m.id + ')">删除</button>'
        + '</div>'
      : '';

    return '<div class="c msg" data-id="' + m.id + '">'
      + '<div class="head"><span class="who">' + esc(m.nickname) + '</span><span class="when">' + fmt(m.time) + '</span></div>'
      + '<div class="body">' + esc(m.content) + '</div>'
      + replies
      + actions
      + '</div>';
  }).join('');
}

async function postMsg() {
  const nickname = $('nickname').value.trim();
  const content = $('content').value.trim();
  if (!nickname || !content) { alert('昵称和内容都不能为空'); return; }
  const res = await fetch('/api/messages', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ nickname, content }),
  });
  if (!res.ok) { alert('发布失败'); return; }
  $('nickname').value = '';
  $('content').value = '';
  load();
}

// ---------------- 管理操作（不再需要密码）----------------

async function replyBox(id) {
  const content = prompt('回复内容：');
  if (!content) return;
  const res = await fetch('/api/messages/' + id + '/reply', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ content }),
  });
  if (res.status === 401) {
    authed = false; renderAuthBar();
    alert('登录已过期，请重新登录');
    return;
  }
  if (!res.ok) { const e = await res.json().catch(() => ({})); alert(e.error || '回复失败'); return; }
  load();
}

async function delBox(id) {
  if (!confirm('确定删除这条留言？')) return;
  const res = await fetch('/api/messages/' + id, { method: 'DELETE' });
  if (res.status === 401) {
    authed = false; renderAuthBar();
    alert('登录已过期，请重新登录');
    return;
  }
  if (!res.ok) { const e = await res.json().catch(() => ({})); alert(e.error || '删除失败'); return; }
  load();
}

// ---------------- 启动 ----------------
(async () => {
  await checkAuth();
  await load();
})();
