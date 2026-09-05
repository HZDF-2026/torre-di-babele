// webui.h — the single-file web UI served at "/". No external assets, no
// dependencies; it talks to the same /v1 API as the CLI/MCP. When serve runs
// with a token, the UI prompts for it and stores it in localStorage.
#ifndef GR_WEBUI_H
#define GR_WEBUI_H

namespace gr {

const char* kWebUiHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>greenroom</title>
<style>
:root{
  --bg:#0e1116;--panel:#161b23;--panel2:#1c2330;--line:#232b3a;
  --fg:#d7dde7;--dim:#8b96a8;--accent:#4fd1a5;--accent2:#7aa2f7;
  --warn:#e0af68;--bad:#f7768e;--mono:ui-monospace,Consolas,Menlo,monospace;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 var(--mono)}
header{display:flex;align-items:center;gap:12px;padding:10px 16px;background:var(--panel);border-bottom:1px solid var(--line)}
header .logo{font-weight:700;color:var(--accent);letter-spacing:.5px}
header .ver{color:var(--dim);font-size:12px}
header input{background:var(--panel2);border:1px solid var(--line);color:var(--fg);border-radius:6px;padding:5px 8px;font:12px var(--mono)}
header button{background:var(--panel2);border:1px solid var(--line);color:var(--fg);border-radius:6px;padding:5px 10px;cursor:pointer;font:12px var(--mono)}
header button:hover{border-color:var(--accent)}
#layout{display:flex;height:calc(100vh - 47px)}
#side{width:220px;min-width:220px;background:var(--panel);border-right:1px solid var(--line);overflow-y:auto;padding:8px}
#side h2{font-size:11px;color:var(--dim);margin:8px 6px;text-transform:uppercase;letter-spacing:1px}
.room-item{padding:6px 10px;border-radius:6px;cursor:pointer;color:var(--fg);word-break:break-all}
.room-item:hover{background:var(--panel2)}
.room-item.sel{background:var(--panel2);color:var(--accent);border-left:2px solid var(--accent)}
#main{flex:1;display:flex;flex-direction:column;min-width:0}
#tabs{display:flex;gap:2px;padding:8px 12px 0;background:var(--panel);border-bottom:1px solid var(--line)}
.tab{padding:6px 14px;cursor:pointer;color:var(--dim);border-radius:6px 6px 0 0}
.tab.sel{color:var(--accent);background:var(--bg);border:1px solid var(--line);border-bottom-color:var(--bg);margin-bottom:-1px}
#content{flex:1;overflow-y:auto;padding:14px 18px}
.msg{display:flex;gap:10px;padding:6px 8px;border-radius:6px;margin-bottom:2px;align-items:baseline}
.msg:hover{background:var(--panel)}
.msg .mid{color:var(--dim);font-size:11px;min-width:44px}
.msg .magent{color:var(--accent2);min-width:90px;max-width:140px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.msg .mtype{font-size:10px;padding:1px 6px;border-radius:4px;border:1px solid var(--line);color:var(--dim);min-width:52px;text-align:center}
.t-say{color:var(--dim)}.t-fact{color:var(--accent);border-color:var(--accent)!important}
.t-ask{color:var(--warn);border-color:var(--warn)!important}
.t-answer{color:var(--accent2);border-color:var(--accent2)!important}
.t-plan{color:var(--accent2)}.t-claim{color:var(--warn)}.t-release{color:var(--dim)}
.t-veto{color:var(--bad);border-color:var(--bad)!important}.t-done{color:var(--accent)}
.t-task{color:var(--accent2);border-color:var(--accent2)!important}
.msg .mcontent{flex:1;white-space:pre-wrap;word-break:break-word}
.msg .mref{color:var(--dim);font-size:11px}
table{border-collapse:collapse;width:100%}
th{color:var(--dim);text-align:left;font-size:11px;text-transform:uppercase;letter-spacing:1px;padding:6px 8px;border-bottom:1px solid var(--line)}
td{padding:6px 8px;border-bottom:1px solid var(--line);vertical-align:top;word-break:break-word}
tr:hover td{background:var(--panel)}
.badge{font-size:10px;padding:1px 8px;border-radius:8px}
.s-open{color:var(--warn);border:1px solid var(--warn)}
.s-claimed{color:var(--accent2);border:1px solid var(--accent2)}
.s-submitted{color:var(--accent);border:1px solid var(--accent)}
.s-done{color:var(--dim);border:1px solid var(--dim)}
button.act{background:var(--panel2);border:1px solid var(--line);color:var(--fg);border-radius:6px;padding:3px 10px;cursor:pointer;font:11px var(--mono)}
button.act:hover{border-color:var(--accent)}
#banner{display:none;background:var(--bad);color:#10131a;padding:6px 16px;font-size:12px}
.empty{color:var(--dim);text-align:center;padding:40px 0}
.evi{color:var(--dim);font-size:12px;margin-top:4px;white-space:pre-wrap}
#newtask{display:flex;gap:6px;margin-bottom:12px}
#newtask input{flex:1;background:var(--panel2);border:1px solid var(--line);color:var(--fg);border-radius:6px;padding:6px 10px;font:12px var(--mono)}
</style>
</head>
<body>
<header>
  <span class="logo">greenroom</span><span class="ver" id="ver"></span>
  <span style="flex:1"></span>
  <input id="me" placeholder="your name" style="width:110px" title="Agent name used for task actions">
  <input id="token" placeholder="token" type="password" style="width:110px" title="Bearer token, only when serve runs with --token">
  <button onclick="connect()">connect</button>
</header>
<div id="banner"></div>
<div id="layout">
  <div id="side">
    <h2>rooms</h2>
    <div id="rooms"><div class="empty">…</div></div>
    <h2 style="cursor:pointer" onclick="promptRoom()" title="Create a room">+ new room</h2>
  </div>
  <div id="main">
    <div id="tabs">
      <div class="tab sel" data-tab="msgs">messages</div>
      <div class="tab" data-tab="claims">claims</div>
      <div class="tab" data-tab="board">board</div>
      <div class="tab" data-tab="tasks">tasks</div>
      <span style="flex:1"></span>
      <div class="tab" id="chainTab" title="Verify the SHA-256 chain">verify</div>
    </div>
    <div id="content"></div>
  </div>
</div>
<script>
"use strict";
let cur = null, since = 0, tab = "msgs", pollTimer = null;
const $ = s => document.querySelector(s);
const esc = s => String(s||"").replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const me = () => $("#me").value || "human";
function token(){ return localStorage.getItem("gr_token") || ""; }
$("#token").value = token();
$("#token").addEventListener("change", e => localStorage.setItem("gr_token", e.target.value));
function banner(msg){ const b=$("#banner"); if(!msg){b.style.display="none";return;} b.textContent=msg; b.style.display="block"; }
async function api(path, opts){
  opts = opts || {};
  opts.headers = Object.assign({"Content-Type":"application/json"}, opts.headers||{});
  if(token()) opts.headers["Authorization"] = "Bearer "+token();
  const r = await fetch(path, opts);
  if(r.status === 401){ banner("401 — token required. Enter it top-right and press connect."); throw new Error("401"); }
  const j = await r.json();
  if(j && j.error){ banner(String(j.error)); throw new Error(String(j.error)); }
  banner(null);
  return j;
}
async function connect(){
  try{
    const st = await api("/v1/status");
    $("#ver").textContent = "v"+st.version+" · "+st.rooms+" rooms";
    const rooms = (await api("/v1/rooms")).rooms || [];
    $("#rooms").innerHTML = rooms.length ? rooms.map(r =>
      `<div class="room-item${r===cur?' sel':''}" onclick="selectRoom('${esc(r)}')">${esc(r)}</div>`).join("")
      : `<div class="empty">no rooms</div>`;
  }catch(e){ if(String(e.message)!=="401") banner("cannot reach greenroom serve — is it running?"); }
}
function promptRoom(){
  const name = prompt("room name (a-z 0-9 . _ -):");
  if(!name) return;
  api("/v1/rooms", {method:"POST", body:JSON.stringify({name})}).then(connect);
}
function selectRoom(r){
  cur = r; since = 0;
  document.querySelectorAll(".room-item").forEach(el => el.classList.toggle("sel", el.textContent === r));
  render();
  longPoll();
}
document.querySelectorAll(".tab[data-tab]").forEach(el => el.onclick = () => {
  tab = el.dataset.tab;
  document.querySelectorAll(".tab[data-tab]").forEach(x => x.classList.toggle("sel", x === el));
  render(); refreshTab();
});
$("#chainTab").onclick = async () => {
  if(!cur) return;
  const v = await api("/v1/rooms/"+cur+"/verify");
  alert(v.ok ? "chain OK — " + "intact" : "CHAIN BROKEN: " + v.error);
};
function fmt(ts){ return new Date(ts).toLocaleTimeString(); }
async function longPoll(){
  clearTimeout(pollTimer);
  const who = cur;
  while(cur === who){
    let msgs = [];
    try{
      msgs = (await api(`/v1/rooms/${who}/wait?since=${since}&timeout_ms=25000`)).messages || [];
    }catch(e){ break; }
    if(cur !== who) return;
    if(msgs.length){
      since = Math.max(since, ...msgs.map(m => m.id));
      if(tab === "msgs") msgs.forEach(addMsg);
    }
  }
}
function addMsg(m){
  const c = $("#content");
  const atBottom = c.scrollTop + c.clientHeight >= c.scrollHeight - 60;
  const div = document.createElement("div");
  div.className = "msg";
  div.innerHTML = `<span class="mid">#${m.id}</span><span class="magent">${esc(m.agent)}</span>`+
    `<span class="mtype t-${esc(m.type)}">${esc(m.type)}</span>`+
    `<span class="mcontent">${esc(m.content)}${m.ref!=null?` <span class="mref">→ #${m.ref}</span>`:""}</span>`;
  c.appendChild(div);
  while(c.children.length > 500) c.removeChild(c.firstChild);
  if(atBottom) c.scrollTop = c.scrollHeight;
}
async function render(){
  const c = $("#content");
  if(!cur){ c.innerHTML = `<div class="empty">select a room</div>`; return; }
  c.innerHTML = "";
  if(tab !== "msgs") return;
  const msgs = (await api(`/v1/rooms/${cur}/messages?since=0&limit=200`)).messages || [];
  since = msgs.length ? msgs[msgs.length-1].id : 0;
  msgs.forEach(addMsg);
  c.scrollTop = c.scrollHeight;
}
async function refreshTab(){
  if(!cur) return;
  const c = $("#content");
  if(tab === "claims"){
    const claims = (await api(`/v1/rooms/${cur}/claims`)).claims || [];
    c.innerHTML = claims.length ? `<table><tr><th>#</th><th>agent</th><th>scope</th><th>expires</th></tr>`+
      claims.map(x=>`<tr><td>${x.id}</td><td>${esc(x.agent)}</td><td>${x.scope.map(esc).join(", ")}</td>`+
        `<td>${x.expiresTs?new Date(x.expiresTs).toLocaleTimeString():"never"}</td></tr>`).join("")+`</table>`
      : `<div class="empty">no active claims</div>`;
  }else if(tab === "board"){
    const es = (await api(`/v1/rooms/${cur}/board`)).entries || [];
    c.innerHTML = es.length ? `<table><tr><th>key</th><th>value</th><th>by</th><th>at</th></tr>`+
      es.map(e=>`<tr><td>${esc(e.key)}</td><td>${esc(e.value)}</td><td>${esc(e.agent)}</td><td>${fmt(e.ts)}</td></tr>`).join("")+`</table>`
      : `<div class="empty">blackboard is empty</div>`;
  }else if(tab === "tasks"){
    const ts = (await api(`/v1/rooms/${cur}/tasks`)).tasks || [];
    c.innerHTML = `<div id="newtask"><input id="nt" placeholder="new task title…">`+
      `<button class="act" onclick="addTask()">add</button></div>`+
      (ts.length ? ts.map(t=>taskRow(t)).join("") : `<div class="empty">no tasks</div>`);
  }
}
function taskRow(t){
  const btns = [];
  if(t.status==="open") btns.push(`<button class="act" onclick="taskAct(${t.id},'claim')">claim</button>`);
  if(t.status==="claimed"&&t.assignee===me()) btns.push(`<button class="act" onclick="taskAct(${t.id},'submit')">submit</button>`);
  if(t.status==="submitted") btns.push(
    `<button class="act" onclick="taskAct(${t.id},'verify',true)">verify</button>`,
    `<button class="act" onclick="taskAct(${t.id},'verify',false)">reject</button>`);
  return `<div class="msg" style="display:block;padding:10px 8px">
    <div><span class="mid">#${t.id}</span> <span class="badge s-${t.status}">${t.status}</span>
      <b style="margin-left:8px">${esc(t.title)}</b>
      ${t.assignee?`<span style="color:var(--accent2);margin-left:8px">@${esc(t.assignee)}</span>`:""}
      <span style="float:right">${btns.join(" ")}</span></div>
    ${t.detail?`<div class="evi">${esc(t.detail)}</div>`:""}
    ${t.evidence?`<div class="evi">evidence: ${esc(t.evidence)}</div>`:""}
  </div>`;
}
async function addTask(){
  const t = $("#nt").value.trim();
  if(!t) return;
  await api(`/v1/rooms/${cur}/tasks`, {method:"POST", body:JSON.stringify({title:t, agent:me()})});
  refreshTab();
}
async function taskAct(id, action, accept){
  try{
    if(action==="claim") await api(`/v1/rooms/${cur}/tasks/${id}/claim`, {method:"POST", body:JSON.stringify({agent:me()})});
    else if(action==="submit"){
      const ev = prompt("evidence (what proves it is done):");
      if(ev==null) return;
      await api(`/v1/rooms/${cur}/tasks/${id}/submit`, {method:"POST", body:JSON.stringify({agent:me(), evidence:ev})});
    }else if(action==="verify") await api(`/v1/rooms/${cur}/tasks/${id}/verify`, {method:"POST", body:JSON.stringify({agent:me(), accept:accept})});
    refreshTab();
  }catch(e){ if(String(e.message)!=="401") alert(e.message||"failed"); }
}
connect();
setInterval(connect, 15000);
</script>
</body>
</html>)HTML";

}  // namespace gr

#endif  // GR_WEBUI_H
