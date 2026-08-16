// 留言板前端逻辑
function $(id){return document.getElementById(id);}

function fmt(t){
  const d = new Date(t);
  const p = n => (n < 10 ? '0' : '') + n;
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate())
    + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
}

// 转义 HTML，防止留言里写 <script> 被当代码执行（XSS）
function esc(s){
  return String(s).replace(/[&<>"']/g, c => (
    {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]
  ));
}

async function load(){
  const res = await fetch('/api/messages');
  const list = await res.json();
  const box = $('list');
  if(!list.length){
    box.innerHTML = '<div class="subtitle">还没有留言，来抢沙发～</div>';
    return;
  }
  box.innerHTML = list.map(m => {
    let replies = '';
    if(m.replies && m.replies.length){
      replies = '<div class="reply-list">' + m.replies.map(r =>
        '<div class="reply"><span class="rwho">博主：</span>' + esc(r.content)
        + '<span class="when"> ' + fmt(r.time) + '</span></div>'
      ).join('') + '</div>';
    }
    return '<div class="c msg" data-id="' + m.id + '">'
      + '<div class="head"><span class="who">' + esc(m.nickname) + '</span><span class="when">' + fmt(m.time) + '</span></div>'
      + '<div class="body">' + esc(m.content) + '</div>'
      + replies
      + '<div class="actions">'
      + '<button onclick="replyBox(' + m.id + ')">回复</button>'
      + '<button class="del" onclick="delBox(' + m.id + ')">删除</button>'
      + '</div></div>';
  }).join('');
}

async function postMsg(){
  const nickname = $('nickname').value.trim();
  const content = $('content').value.trim();
  if(!nickname || !content){ alert('昵称和内容都不能为空'); return; }
  const res = await fetch('/api/messages', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({nickname, content})
  });
  if(!res.ok){ alert('发布失败'); return; }
  $('nickname').value = ''; $('content').value = '';
  load();
}

function askPassword(){ return prompt('管理员密码：') || ''; }

async function replyBox(id){
  const password = askPassword();
  if(!password) return;
  const content = prompt('回复内容：');
  if(!content) return;
  const res = await fetch('/api/messages/' + id + '/reply', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({content, password})
  });
  if(!res.ok){ const e = await res.json(); alert(e.error || '回复失败'); return; }
  load();
}

async function delBox(id){
  if(!confirm('确定删除这条留言？')) return;
  const password = askPassword();
  if(!password) return;
  const res = await fetch('/api/messages/' + id, {
    method:'DELETE',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({password})
  });
  if(!res.ok){ const e = await res.json(); alert(e.error || '删除失败'); return; }
  load();
}

load();
