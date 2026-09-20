// 留言板前端逻辑
//
// 依赖 auth.js 提供的 api / requireLogin / renderUserBar
//
// 权限：浏览和发言需登录；回复和删除需管理员。

let me = null;   // 当前登录账号

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

// ---------------- 留言列表 ----------------

async function load() {
  const r = await api('/api/messages');
  if (!r.ok) {
    if (r.status === 401) return location.replace('/login?next=' + encodeURIComponent('/messages'));
    $('list').innerHTML = '<div class="subtitle">' + esc(r.error) + '</div>';
    return;
  }

  const list = r.data;
  const box = $('list');
  if (!list.length) {
    box.innerHTML = '<div class="subtitle">还没有留言，来抢沙发～</div>';
    return;
  }

  const isAdmin = me && me.role === 'admin';

  box.innerHTML = list.map(m => {
    let replies = '';
    if (m.replies && m.replies.length) {
      replies = '<div class="reply-list">' + m.replies.map(x =>
        '<div class="reply"><span class="rwho">博主：</span>' + esc(x.content)
        + '<span class="when"> ' + fmt(x.time) + '</span></div>'
      ).join('') + '</div>';
    }

    // 回复/删除只有管理员能看到
    const actions = isAdmin
      ? '<div class="actions">'
        + '<button onclick="replyBox(' + m.id + ')">回复</button>'
        + '<button class="del" onclick="delBox(' + m.id + ')">删除</button>'
        + '</div>'
      : '';

    return '<div class="c msg" data-id="' + m.id + '">'
      + '<div class="head"><span class="who">' + esc(m.nickname) + '</span><span class="when">' + fmt(m.time) + '</span></div>'
      + '<div class="body">' + esc(m.content) + '</div>'
      + replies + actions
      + '</div>';
  }).join('');
}

// ---------------- 发留言 ----------------
//
// 昵称不用输入 —— 服务端取登录账号的昵称，防止冒名

async function postMsg() {
  const content = $('content').value.trim();
  if (!content) { alert('留言内容不能为空'); return; }

  const r = await api('/api/messages', { content });
  if (!r.ok) {
    if (r.status === 401) return location.replace('/login?next=' + encodeURIComponent('/messages'));
    alert(r.error);
    return;
  }
  $('content').value = '';
  load();
}

// ---------------- 管理操作 ----------------

async function replyBox(id) {
  const content = prompt('回复内容：');
  if (!content) return;

  const r = await api('/api/messages/' + id + '/reply', { content });
  if (!r.ok) { alert(r.error); return; }
  load();
}

async function delBox(id) {
  if (!confirm('确定删除这条留言？')) return;

  const res = await fetch('/api/messages/' + id, { method: 'DELETE' });
  if (!res.ok) {
    const e = await res.json().catch(() => ({}));
    alert(e.error || '删除失败');
    return;
  }
  load();
}

// ---------------- 启动 ----------------

(async () => {
  me = await requireLogin();      // 未登录会跳转到登录页
  if (!me) return;

  renderUserBar('userbar', me);

  // 非管理员提示一下能做什么
  if (me.role !== 'admin') {
    const hint = $('rolehint');
    if (hint) hint.textContent = '你能发言，但不能回复或删除别人的留言';
  }

  await load();
})();
