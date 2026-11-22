/* Lightweight Web UI + REST API + Browser OTA on http://192.168.4.1 */
#include "web_ui.h"
#include "ble_provision.h"
#include "ota_updater.h"
#include "stadia_opus.h"
#include "stadia_transport.h"
#include "usb_audio.h"
#include "usb_gamepad.h"
#include "wifi_ap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define TAG "WEBUI"

static uint32_t get_req_client_ip4(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return 0;
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &slen) != 0) return 0;
    if (ss.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    } else if (ss.ss_family == AF_INET6) {
        return ((struct sockaddr_in6 *)&ss)->sin6_addr.un.u32_addr[3];
    }
    return 0;
}

static void delayed_reboot_task(void *arg) {
    bool to_boot = (arg != NULL);
    vTaskDelay(pdMS_TO_TICKS(500));
    ota_updater_reboot_clean(to_boot);
    vTaskDelete(NULL);
}

static void schedule_reboot(bool to_bootloader) {
    xTaskCreate(delayed_reboot_task, "web_reboot", 3072,
                to_bootloader ? (void *)1 : NULL, 5, NULL);
}

static const char k_index_html[] =
"<!doctype html><html lang=en><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Stadia Wireless Adapter</title><style>"
":root{--bg:#0f1117;--card:#191d27;--bd:#2a3040;--fg:#e8ecf5;--mut:#8e98ad;--acc:#ff6b35;--ok:#34d399;--warn:#f59e0b;--bad:#ef4444}"
"*{box-sizing:border-box}body{margin:0;padding:16px;font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--fg);max-width:720px;margin-inline:auto}"
"h1{font-size:1.35rem;margin:0 0 4px}h2{font-size:1rem;margin:0 0 12px;color:#cfd6e6}"
".sub{color:var(--mut);font-size:.85rem;margin-bottom:16px}"
".card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:14px;margin-bottom:12px}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(135px,1fr));gap:10px}"
".stat{background:#12151e;border:1px solid var(--bd);border-radius:8px;padding:10px}"
".stat .k{font-size:.75rem;color:var(--mut);margin-bottom:4px}.stat .v{font-size:1.05rem;font-weight:600}"
".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:10px}"
".row:last-child{margin-bottom:0}"
"label{font-size:.85rem;color:var(--mut);min-width:120px}"
"select,input[type=text],input[type=password]{flex:1;min-width:160px;background:#10131b;color:var(--fg);border:1px solid var(--bd);border-radius:8px;padding:8px 10px;font-size:.9rem}"
"button{background:var(--acc);color:#fff;border:0;border-radius:8px;padding:8px 14px;font-weight:600;font-size:.88rem;cursor:pointer}"
"button.sec{background:#283044}button.warn{background:#b45309}button.bad{background:#b91c1c}button:disabled{opacity:.5;cursor:default}"
".bar{height:10px;background:#10131b;border-radius:99px;overflow:hidden;border:1px solid var(--bd);margin-top:8px}"
".bar>div{height:100%;width:0;background:var(--acc);transition:width .15s}"
".msg{font-size:.85rem;margin-top:8px;color:var(--ok)}.msg.err{color:var(--bad)}"
".pill{display:inline-block;padding:2px 8px;border-radius:99px;font-size:.75rem;font-weight:600;background:#262d3f}"
".pill.ok{background:#064e3b;color:#6ee7b7}.pill.wait{background:#3f2e12;color:#fcd34d}.pill.exp{background:#3b1d5e;color:#d8b4fe}"
"small{color:var(--mut);font-size:.78rem;display:block;margin-top:4px}"
".vgrid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:10px}"
"@media(max-width:520px){.vgrid{grid-template-columns:1fr}}"
".stick-box{display:flex;align-items:center;gap:12px;background:#12151e;border:1px solid var(--bd);border-radius:10px;padding:10px}"
".pad-ring{position:relative;width:84px;height:84px;border-radius:50%;background:#0b0d13;border:1px solid #333b4f;flex-shrink:0}"
".pad-ring::before,.pad-ring::after{content:'';position:absolute;background:#222838}"
".pad-ring::before{left:50%;top:0;bottom:0;width:1px}.pad-ring::after{top:50%;left:0;right:0;height:1px}"
".pad-dot{position:absolute;width:12px;height:12px;border-radius:50%;background:var(--acc);transform:translate(-50%,-50%);left:50%;top:50%;box-shadow:0 0 8px var(--acc)}"
".bgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(62px,1fr));gap:6px;margin-top:10px}"
".bchip{background:#12151e;border:1px solid var(--bd);border-radius:7px;padding:6px 4px;text-align:center;font-size:.76rem;font-weight:600;color:var(--mut);user-select:none;transition:background .05s,color .05s,border-color .05s}"
".bchip.on{background:var(--acc);color:#fff;border-color:var(--acc);box-shadow:0 0 10px rgba(255,107,53,.45)}"
"</style></head><body>"
"<h1>Stadia Wireless Adapter <span class='pill exp'>EXPERIMENTAL</span></h1>"
"<div class=sub>Firmware <span id=ver>...</span> &middot; Slot <span id=slot>...</span> &middot; <span id=conn class='pill wait'>Checking...</span></div>"

"<div class=card><div class=grid>"
"<div class=stat><div class=k>Battery</div><div class=v id=batt>--</div></div>"
"<div class=stat><div class=k>USB Mode</div><div class=v id=mode>--</div></div>"
"<div class=stat><div class=k>3.5 mm Headset</div><div class=v id=hs>--</div></div>"
"<div class=stat><div class=k>Microphone</div><div class=v id=mic>--</div></div>"
"</div>"
"<div id=pSlots style='display:none;margin-top:10px;gap:6px;flex-wrap:wrap'></div>"
"<div id=otaMsg class=msg></div></div>"

"<div class=card><div class=row style='justify-content:space-between'>"
"<h2 style='margin:0'>Live Controller Visualizer</h2>"
"<div style='display:flex;gap:8px;align-items:center'>"
"<select id=vizSlot style='display:none;min-width:90px;padding:6px 8px;font-size:.82rem'>"
"<option value=0>Player 1</option><option value=1>Player 2</option><option value=2>Player 3</option><option value=3>Player 4</option></select>"
"<button id=btnViz class=sec onclick='toggleViz()'>Start Live Monitor</button>"
"<button class=sec onclick='testRumble()'>Test Rumble</button></div></div>"
"<div id=vizBody style='display:none;margin-top:10px'>"
"<div class=vgrid>"
"<div class=stick-box><div><div class=k style='font-size:.75rem;color:var(--mut)'>L2 Trigger</div>"
"<div style='font-weight:600;font-size:.95rem' id=ltVal>0 / 255 (0%)</div></div>"
"<div style='flex:1'><div class=bar style='margin:0'><div id=ltBar></div></div></div></div>"
"<div class=stick-box><div><div class=k style='font-size:.75rem;color:var(--mut)'>R2 Trigger</div>"
"<div style='font-weight:600;font-size:.95rem' id=rtVal>0 / 255 (0%)</div></div>"
"<div style='flex:1'><div class=bar style='margin:0'><div id=rtBar></div></div></div></div>"
"<div class=stick-box><div class=pad-ring><div id=lDot class=pad-dot></div></div>"
"<div><div style='font-size:.78rem;color:var(--mut)'>Left Stick</div>"
"<div style='font-family:monospace;font-size:.85rem;margin-top:2px' id=lTxt>X:     0\nY:     0</div></div></div>"
"<div class=stick-box><div class=pad-ring><div id=rDot class=pad-dot></div></div>"
"<div><div style='font-size:.78rem;color:var(--mut)'>Right Stick</div>"
"<div style='font-family:monospace;font-size:.85rem;margin-top:2px' id=rTxt>X:     0\nY:     0</div></div></div>"
"</div>"
"<div class=bgrid id=btnGrid></div>"
"<small>Polls live input at 25 Hz only while open (0% Wi-Fi traffic when closed).</small>"
"</div></div>"

"<div class=card><h2>Gamepad &amp; Audio</h2>"
"<div class=row><label>USB Mode</label><select id=selMode>"
"<option value=0>Stadia HID (18d1:9400 + Audio)</option>"
"<option value=1>XInput / Xbox 360 (1209:9401 + Audio)</option>"
"<option value=2>DualShock 4 v2 (054c:09cc + Audio)</option>"
"<option value=3>Switch Pro (057e:2009, HID-only)</option>"
"<option value=4>PS3 / DualShock 3 (054c:0268, HID-only)</option></select>"
"<button onclick='saveMode()'>Switch &amp; Reboot</button></div>"
"<div class=row><label>Players <span class='pill exp'>EXP</span></label><select id=selPlayers>"
"<option value=1>1 Player (Default + 48 kHz USB Audio)</option>"
"<option value=2>2 Players (Experimental Co-Op, No Audio)</option>"
"<option value=3>3 Players (Experimental Co-Op, No Audio)</option>"
"<option value=4>4 Players (Experimental Co-Op, No Audio)</option></select>"
"<button onclick='savePlayers()'>Apply &amp; Reboot</button></div>"
"<div class=row><label>Microphone</label><button id=btnMute class=sec onclick='toggleMute()'>Toggle Mute</button></div>"
"<div class=row><label>Mic Gain</label><select id=selGain onchange='postSet({mic_gain_q8:+this.value})'>"
"<option value=256>1.0x</option><option value=384>1.5x</option><option value=512>2.0x</option><option value=640>2.5x</option></select></div>"
"<div class=row><label>Stick Deadzone</label><select id=selDz onchange='postSet({stick_dz:+this.value})'>"
"<option value=0>0% (Raw)</option><option value=1310>4%</option><option value=2621>8%</option></select></div>"
"<div class=row><label>Wi-Fi TX Power</label><select id=selTx onchange='postSet({wifi_tx_qdbm:+this.value})'>"
"<option value=32>+8 dBm (Coolest)</option><option value=44>+11 dBm</option><option value=52>+13 dBm (Default)</option><option value=64>+16 dBm</option><option value=80>+20 dBm (Max Range)</option></select></div>"
"<small>Experimental 2–4 Player Co-Op dedicates all 4 hardware USB IN endpoints to independent XInput or Stadia HID gamepads on a single dongle (disables UAC1 USB audio while &gt;1 player is active).</small>"
"<div id=setMsg class=msg></div></div>"

"<div class=card><h2>Auto-Provision Stadia Controller (BLE)</h2>"
"<div class=row><label>Status</label><span id=bleSt style='font-size:.9rem'>...</span></div>"
"<div class=row><label>Auto-Pair Mode</label><select id=selBle onchange='postSet({ble_auto:this.value===\"1\"},\"bleMsg\")'>"
"<option value=1>Enabled (auto-pairs blinking-orange controllers)</option><option value=0>Disabled (manual only)</option></select>"
"<button onclick='scanBle()'>Scan &amp; Pair Now</button></div>"
"<small>Put any Wi-Fi Stadia Controller into orange-blink setup mode near the dongle and it will automatically receive this dongle's Wi-Fi credentials — no phone app or PC needed.</small>"
"<div id=bleMsg class=msg></div></div>"

"<div class=card><h2>Browser Firmware Update (OTA)</h2>"
"<div class=row><input type=file id=otaFile accept=.bin><button id=btnOta onclick='uploadOta()'>Upload &amp; Flash</button></div>"
"<div class=bar><div id=otaBar></div></div>"
"<small>Select <b>stadiacast-esp32.bin</b> (app binary, NOT *-merged.bin). Automatic rollback protects against bad flashes.</small>"
"<div id=upMsg class=msg></div></div>"

"<div class=card><h2>Home Wi-Fi / LAN Auto-OTA</h2>"
"<div class=row><label>Home Wi-Fi SSID</label><input id=staSsid type=text maxlength=32 placeholder='Your router SSID'></div>"
"<div class=row><label>Home Wi-Fi Pass</label><input id=staPass type=password maxlength=63 placeholder='Leave blank to keep saved'></div>"
"<div class=row><label>Custom OTA URL</label><input id=otaUrl type=text maxlength=190 placeholder='Blank = GitHub latest release'></div>"
"<div class=row><button class=sec onclick='saveSta()'>Save Wi-Fi</button>"
"<button onclick='runStaOta()'>Update Now over Home Wi-Fi</button></div>"
"<small>Hotkey: hold <b>Assistant + Capture + B</b> (2 s) to auto-update anytime. Only connects during update; zero gaming impact.</small>"
"<div id=staMsg class=msg></div></div>"

"<div class=card><h2>SoftAP Wi-Fi Credentials</h2>"
"<div class=row><label>Dongle SSID</label><input id=apSsid type=text maxlength=32 placeholder='STADIA_ESP32'></div>"
"<div class=row><label>Dongle Pass</label><input id=apPass type=password maxlength=63 placeholder='8-63 chars (blank to keep)'></div>"
"<div class=row><button class=sec onclick='saveAp()'>Save &amp; Reboot</button>"
"<button class=warn onclick='resetAp()'>Reset to STADIA_ESP32</button></div>"
"<div id=apMsg class=msg></div></div>"

"<div class=card><h2>System</h2><div class=row>"
"<button class=sec onclick='reboot(false)'>Reboot Dongle</button>"
"<button class=bad onclick='reboot(true)'>Enter USB Bootloader</button>"
"</div><small>USB Bootloader enters ESP32-S3 ROM flash mode without touching the B button (or hold <b>Assistant + Capture + Y</b> for 2 s). <span id=mem></span></small></div>"

"<script>"
"let curMute=false,initDone=false;"
"function msg(id,t,e){const el=document.getElementById(id);el.textContent=t;el.className='msg'+(e?' err':'');}"
"async function poll(){try{const r=await fetch('/api/status');const d=await r.json();"
"document.getElementById('ver').textContent='v'+d.version+'-exp';"
"document.getElementById('slot').textContent=d.slot;"
"const mp=d.max_players||1;"
"const c=document.getElementById('conn');"
"if(mp>1){c.textContent=(d.connected_count||0)+' / '+mp+' Controllers Connected';c.className='pill '+((d.connected_count>0)?'ok':'wait');}"
"else{c.textContent=d.ready?'Controller Connected':'Waiting for Controller';c.className='pill '+(d.ready?'ok':'wait');}"
"document.getElementById('batt').textContent=d.battery_pct>=0?d.battery_pct+'%':'Waiting...';"
"const MODES=['Stadia HID','XInput','DualShock 4','Switch Pro','PS3 (DS3)'];"
"document.getElementById('mode').textContent=(MODES[d.usb_mode]||'Unknown')+(mp>1?' ('+mp+'P)':'');"
"document.getElementById('hs').textContent=mp>1?'Disabled (>1P)':(d.ready?(d.headset?'Plugged In':'Built-in Mic'):'--');"
"curMute=!!d.mic_muted;document.getElementById('mic').textContent=mp>1?'Disabled (>1P)':(curMute?'Muted':'Live');"
"document.getElementById('btnMute').textContent=curMute?'Unmute Mic':'Mute Mic';"
"const ps=document.getElementById('pSlots');const vs=document.getElementById('vizSlot');"
"if(mp>1&&Array.isArray(d.players)){ps.style.display='flex';vs.style.display='inline-block';"
"ps.innerHTML=d.players.map((p,i)=>`<span class='pill ${p.ready?\"ok\":\"wait\"}'>P${i+1}: ${p.ready?(p.batt>=0?p.batt+\"%\":\"Connected\"):\"Waiting\"}</span>`).join('');"
"}else{ps.style.display='none';vs.style.display='none';}"
"document.getElementById('otaMsg').textContent=d.ota_msg?('Last OTA: '+d.ota_msg):'';"
"document.getElementById('bleSt').textContent=d.ble_status||'--';"
"const du=d.total_dram?Math.round((d.total_dram-d.free_dram)*100/d.total_dram):0;"
"const pu=d.total_psram?((d.total_psram-d.free_psram)*100/d.total_psram).toFixed(1):0;"
"document.getElementById('mem').textContent='Up '+d.uptime_s+'s · DRAM '+du+'% used ('+Math.round(d.free_dram/1024)+'KB free) · PSRAM '+pu+'% used ('+(d.free_psram/1048576).toFixed(1)+'MB free)';"
"if(!initDone){initDone=true;document.getElementById('selMode').value=d.usb_mode;"
"document.getElementById('selPlayers').value=mp;"
"document.getElementById('selGain').value=d.mic_gain_q8;document.getElementById('selDz').value=d.stick_dz;"
"document.getElementById('selTx').value=d.wifi_tx_qdbm;document.getElementById('apSsid').value=d.ap_ssid;"
"document.getElementById('selBle').value=d.ble_auto?'1':'0';"
"document.getElementById('staSsid').value=d.sta_ssid||'';document.getElementById('otaUrl').value=d.ota_url||'';}"
"}catch(e){}}"
"async function postSet(obj,mid='setMsg'){try{const r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(obj)});"
"const d=await r.json();msg(mid,d.msg||(d.ok?'Saved':'Failed'),!d.ok);poll();}catch(e){msg(mid,'Request failed',true);}}"
"function toggleMute(){postSet({mic_muted:!curMute});}"
"function saveMode(){postSet({usb_mode:+document.getElementById('selMode').value});}"
"function savePlayers(){postSet({max_players:+document.getElementById('selPlayers').value});}"
"function saveSta(){postSet({sta_ssid:document.getElementById('staSsid').value.trim(),sta_pass:document.getElementById('staPass').value,ota_url:document.getElementById('otaUrl').value.trim()},'staMsg');}"
"function saveAp(){postSet({ap_ssid:document.getElementById('apSsid').value.trim(),ap_pass:document.getElementById('apPass').value},'apMsg');}"
"function resetAp(){if(confirm('Reset SoftAP back to STADIA_ESP32 / stadiacast and reboot?'))postSet({ap_reset:true},'apMsg');}"
"async function runStaOta(){if(!confirm('Reboot into Home Wi-Fi Auto-OTA mode now?'))return;"
"try{const r=await fetch('/api/ota-sta',{method:'POST'});const d=await r.json();msg('staMsg',d.msg||'Starting...',!d.ok);}catch(e){msg('staMsg','Error',true);}}"
"async function scanBle(){try{const r=await fetch('/api/ble-prov',{method:'POST'});const d=await r.json();msg('bleMsg',d.msg||'Scanning...',!d.ok);poll();}catch(e){msg('bleMsg','Error',true);}}"
"async function reboot(bl){if(bl&&!confirm('Enter USB ROM Bootloader mode? (Unplug/replug to exit)'))return;"
"await fetch('/api/reboot'+(bl?'?mode=bootloader':''),{method:'POST'});alert(bl?'Dongle entering USB Bootloader...':'Dongle rebooting...');}"
"function uploadOta(){const f=document.getElementById('otaFile').files[0];if(!f){msg('upMsg','Pick stadiacast-esp32.bin first',true);return;}"
"const btn=document.getElementById('btnOta');btn.disabled=true;msg('upMsg','Uploading '+Math.round(f.size/1024)+' KB...');"
"const x=new XMLHttpRequest();x.open('POST','/api/ota');"
"x.upload.onprogress=e=>{if(e.lengthComputable)document.getElementById('otaBar').style.width=Math.round(e.loaded*100/e.total)+'%';};"
"x.onload=()=>{btn.disabled=false;try{const d=JSON.parse(x.responseText);msg('upMsg',d.msg||(d.ok?'Flashed! Rebooting...':'Failed'),!d.ok);"
"if(d.ok)setTimeout(()=>location.reload(),6000);}catch(e){msg('upMsg','HTTP '+x.status+': '+x.responseText,x.status!==200);}};"
"x.onerror=()=>{btn.disabled=false;msg('upMsg','Upload interrupted',true);};x.send(f);}"
"const BTNS=[[0,'Up'],[1,'Down'],[2,'Left'],[3,'Right'],[8,'L1'],[9,'R1'],[6,'L3'],[7,'R3'],[5,'Options'],[4,'Menu'],[10,'Asst'],[11,'Capture'],[16,'Stadia'],[12,'A'],[13,'B'],[14,'X'],[15,'Y']];"
"const bg=document.getElementById('btnGrid');"
"BTNS.forEach(([b,n])=>{const e=document.createElement('div');e.className='bchip';e.id='b_'+b;e.textContent=n;bg.appendChild(e);});"
"let vizOn=false,vizTmr=null,vizBusy=false;"
"function toggleViz(){vizOn=!vizOn;document.getElementById('vizBody').style.display=vizOn?'block':'none';"
"const b=document.getElementById('btnViz');b.textContent=vizOn?'Stop Live Monitor':'Start Live Monitor';b.className=vizOn?'':'sec';"
"if(vizOn){pollViz();vizTmr=setInterval(pollViz,40);}else if(vizTmr){clearInterval(vizTmr);vizTmr=null;}}"
"async function pollViz(){if(!vizOn||vizBusy||document.hidden)return;vizBusy=true;try{"
"const sl=document.getElementById('vizSlot').value||0;"
"const r=await fetch('/api/input?slot='+sl);const d=await r.json();"
"document.getElementById('ltVal').textContent=d.lt+' / 255 ('+Math.round(d.lt*100/255)+'%)';"
"document.getElementById('rtVal').textContent=d.rt+' / 255 ('+Math.round(d.rt*100/255)+'%)';"
"document.getElementById('ltBar').style.width=(d.lt*100/255)+'%';"
"document.getElementById('rtBar').style.width=(d.rt*100/255)+'%';"
"document.getElementById('lDot').style.left=(50+d.lx*42/32768)+'%';"
"document.getElementById('lDot').style.top=(50-d.ly*42/32768)+'%';"
"document.getElementById('rDot').style.left=(50+d.rx*42/32768)+'%';"
"document.getElementById('rDot').style.top=(50-d.ry*42/32768)+'%';"
"document.getElementById('lTxt').textContent='X: '+d.lx+' ('+Math.round(d.lx*100/32767)+'%)\\nY: '+d.ly+' ('+Math.round(d.ly*100/32767)+'%)';"
"document.getElementById('rTxt').textContent='X: '+d.rx+' ('+Math.round(d.rx*100/32767)+'%)\\nY: '+d.ry+' ('+Math.round(d.ry*100/32767)+'%)';"
"BTNS.forEach(([bit])=>{const el=document.getElementById('b_'+bit);if(el)el.className='bchip'+((d.b&(1<<bit))?' on':'');});"
"}catch(e){}vizBusy=false;}"
"async function testRumble(){const sl=document.getElementById('vizSlot').value||0;try{await fetch('/api/rumble?slot='+sl,{method:'POST'});}catch(e){}}"
"poll();setInterval(poll,3000);"
"</script></body></html>";

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, k_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t get_api_status(httpd_req_t *req) {
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char ap_ssid[33] = {0}, sta_ssid[33] = {0}, ota_url[192] = {0}, ota_msg[96] = {0}, ble_st[96] = {0};
    int8_t tx_qdbm = WIFI_AP_DEFAULT_TX_QDBM;

    wifi_ap_get_config(ap_ssid, sizeof(ap_ssid), &tx_qdbm);
    ota_updater_get_config(sta_ssid, sizeof(sta_ssid), ota_url, sizeof(ota_url));
    ota_updater_get_last_msg(ota_msg, sizeof(ota_msg));
    ble_provision_get_status(ble_st, sizeof(ble_st));

    cJSON *root = cJSON_CreateObject();
    if (!root) return send_json(req, "500 Internal Server Error", "{\"ok\":false}");

    uint32_t client_ip = get_req_client_ip4(req);
    int rssi0 = wifi_ap_get_peer_rssi(stadia_transport_get_slot_peer_ip(0), client_ip);
    uint8_t max_p = usb_gamepad_get_max_players();
    cJSON_AddStringToObject(root, "version", app ? app->version : "unknown");
    cJSON_AddStringToObject(root, "slot", running ? running->label : "unknown");
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "free_dram", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "total_dram", (double)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "free_psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "total_psram", (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(root, "ready", stadia_transport_is_ready());
    cJSON_AddNumberToObject(root, "battery_pct", stadia_transport_get_battery_pct());
    cJSON_AddNumberToObject(root, "rssi_dbm", rssi0);
    cJSON_AddBoolToObject(root, "headset", stadia_transport_is_headset_plugged());
    cJSON_AddNumberToObject(root, "usb_mode", (int)usb_gamepad_get_mode());
    cJSON_AddNumberToObject(root, "max_players", (int)max_p);
    cJSON_AddNumberToObject(root, "connected_count", stadia_transport_get_connected_count());
    cJSON_AddBoolToObject(root, "game_hs", stadia_transport_is_slot_game_ready(0));

    cJSON *parr = cJSON_CreateArray();
    if (parr) {
        for (int i = 0; i < (int)max_p; i++) {
            cJSON *po = cJSON_CreateObject();
            if (po) {
                int prssi = wifi_ap_get_peer_rssi(stadia_transport_get_slot_peer_ip(i), client_ip);
                cJSON_AddBoolToObject(po, "ready", stadia_transport_is_slot_ready(i));
                cJSON_AddNumberToObject(po, "batt", stadia_transport_get_slot_battery_pct(i));
                cJSON_AddNumberToObject(po, "rssi", prssi);
                cJSON_AddItemToArray(parr, po);
            }
        }
        cJSON_AddItemToObject(root, "players", parr);
    }

    cJSON_AddBoolToObject(root, "mic_muted", usb_audio_is_mic_muted());
    cJSON_AddNumberToObject(root, "mic_gain_q8", (int)stadia_opus_get_mic_gain_q8());
    cJSON_AddNumberToObject(root, "stick_dz", (int)usb_gamepad_get_deadzone());
    cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
    cJSON_AddNumberToObject(root, "wifi_tx_qdbm", (int)tx_qdbm);
    cJSON_AddStringToObject(root, "sta_ssid", sta_ssid);
    cJSON_AddStringToObject(root, "ota_url", ota_url);
    cJSON_AddStringToObject(root, "ota_msg", ota_msg);
    cJSON_AddBoolToObject(root, "ble_auto", ble_provision_get_auto());
    cJSON_AddStringToObject(root, "ble_status", ble_st);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return send_json(req, "500 Internal Server Error", "{\"ok\":false}");
    esp_err_t rc = send_json(req, "200 OK", out);
    free(out);
    return rc;
}

static esp_err_t post_api_settings(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 1024) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"msg\":\"Invalid payload size\"}");
    }
    char *body = calloc(1, (size_t)req->content_len + 1);
    if (!body) return send_json(req, "500 Internal Server Error", "{\"ok\":false}");

    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(body);
            return send_json(req, "400 Bad Request", "{\"ok\":false,\"msg\":\"Read failed\"}");
        }
        got += r;
    }

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return send_json(req, "400 Bad Request", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");

    bool need_reboot = false;
    const char *msg = "Saved";

    cJSON *it;
    if ((it = cJSON_GetObjectItem(j, "mic_muted")) && cJSON_IsBool(it)) {
        usb_audio_set_mic_muted(cJSON_IsTrue(it));
        msg = cJSON_IsTrue(it) ? "Mic muted" : "Mic unmuted";
    }
    if ((it = cJSON_GetObjectItem(j, "mic_gain_q8")) && cJSON_IsNumber(it)) {
        stadia_opus_set_mic_gain_q8((uint16_t)it->valueint);
        msg = "Mic gain updated";
    }
    if ((it = cJSON_GetObjectItem(j, "stick_dz")) && cJSON_IsNumber(it)) {
        usb_gamepad_set_deadzone((uint16_t)it->valueint);
        msg = "Stick deadzone updated";
    }
    if ((it = cJSON_GetObjectItem(j, "wifi_tx_qdbm")) && cJSON_IsNumber(it)) {
        wifi_ap_set_tx_power((int8_t)it->valueint);
        msg = "Wi-Fi TX power updated";
    }
    if ((it = cJSON_GetObjectItem(j, "ble_auto")) && cJSON_IsBool(it)) {
        ble_provision_set_auto(cJSON_IsTrue(it));
        msg = cJSON_IsTrue(it) ? "BLE auto-provisioning enabled" : "BLE auto-provisioning disabled";
    }
    if ((it = cJSON_GetObjectItem(j, "usb_mode")) && cJSON_IsNumber(it)) {
        if (it->valueint < 0 || it->valueint >= (int)USB_GAMEPAD_MODE_COUNT) {
            cJSON_Delete(j);
            return send_json(req, "400 Bad Request", "{\"ok\":false,\"msg\":\"Invalid USB mode\"}");
        }
        usb_gamepad_mode_t m = (usb_gamepad_mode_t)it->valueint;
        usb_gamepad_save_mode(m);
        need_reboot = true;
        static char s_mode_msg[64];
        snprintf(s_mode_msg, sizeof(s_mode_msg), "Switching to %s, rebooting...", usb_gamepad_mode_name(m));
        msg = s_mode_msg;
    }
    if ((it = cJSON_GetObjectItem(j, "max_players")) && cJSON_IsNumber(it)) {
        if (it->valueint < 1 || it->valueint > USB_MAX_PLAYERS) {
            cJSON_Delete(j);
            return send_json(req, "400 Bad Request", "{\"ok\":false,\"msg\":\"Players must be 1..4\"}");
        }
        usb_gamepad_save_max_players((uint8_t)it->valueint);
        need_reboot = true;
        static char s_plr_msg[64];
        snprintf(s_plr_msg, sizeof(s_plr_msg), "Configuring %d-Player mode, rebooting...", it->valueint);
        msg = s_plr_msg;
    }
    if ((it = cJSON_GetObjectItem(j, "ap_reset")) && cJSON_IsTrue(it)) {
        wifi_ap_save_credentials("", "");
        need_reboot = true;
        msg = "SoftAP reset to STADIA_ESP32, rebooting...";
    } else if ((it = cJSON_GetObjectItem(j, "ap_ssid")) && cJSON_IsString(it)) {
        cJSON *p = cJSON_GetObjectItem(j, "ap_pass");
        const char *pass = (p && cJSON_IsString(p)) ? p->valuestring : "";
        if (!wifi_ap_save_credentials(it->valuestring, pass)) {
            cJSON_Delete(j);
            return send_json(req, "400 Bad Request", "{\"ok\":false,\"msg\":\"SSID 1-32 chars, pass 8-63 chars\"}");
        }
        need_reboot = true;
        msg = "SoftAP credentials saved, rebooting...";
    }
    if (cJSON_GetObjectItem(j, "sta_ssid") || cJSON_GetObjectItem(j, "ota_url")) {
        cJSON *ss = cJSON_GetObjectItem(j, "sta_ssid");
        cJSON *sp = cJSON_GetObjectItem(j, "sta_pass");
        cJSON *ou = cJSON_GetObjectItem(j, "ota_url");
        if (!ota_updater_save_config(
                (ss && cJSON_IsString(ss)) ? ss->valuestring : NULL,
                (sp && cJSON_IsString(sp)) ? sp->valuestring : NULL,
                (ou && cJSON_IsString(ou)) ? ou->valuestring : NULL)) {
            cJSON_Delete(j);
            return send_json(req, "400 Bad Request", "{\"ok\":false,\"msg\":\"Invalid Home Wi-Fi or URL\"}");
        }
        msg = "Home Wi-Fi / OTA config saved";
    }

    cJSON_Delete(j);

    char resp[160];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"reboot\":%s,\"msg\":\"%s\"}",
             need_reboot ? "true" : "false", msg);
    esp_err_t rc = send_json(req, "200 OK", resp);
    if (need_reboot) schedule_reboot(false);
    return rc;
}

static esp_err_t post_api_ota(httpd_req_t *req) {
    int total = req->content_len;
    if (total < 65536 || total > 0x300000) {
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"msg\":\"Invalid image size (upload stadiacast-esp32.bin)\"}");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"msg\":\"No OTA partition found\"}");
    }

    uint8_t *buf = malloc(4096);
    if (!buf) return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"msg\":\"OOM\"}");

    ESP_LOGI(TAG, "Browser OTA start: %d bytes -> %s", total, part->label);
    esp_ota_handle_t handle = 0;
    bool began = false;
    int remaining = total;
    int received = 0;
    const char *err_msg = NULL;

    int timeouts = 0;
    while (remaining > 0) {
        int to_read = remaining > 4096 ? 4096 : remaining;
        int r = httpd_req_recv(req, (char *)buf, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= 3) {
                err_msg = "Upload timed out";
                break;
            }
            continue;
        }
        timeouts = 0;
        if (r <= 0) {
            err_msg = "Upload interrupted";
            break;
        }

        if (!began) {
            if (r < 36 || buf[0] != 0xE9) {
                err_msg = "Not a valid ESP32 firmware binary";
                break;
            }
            uint32_t magic = 0;
            memcpy(&magic, buf + 32, sizeof(magic));
            if (magic != ESP_APP_DESC_MAGIC_WORD) {
                err_msg = "Wrong file! Upload stadiacast-esp32.bin (NOT *-merged.bin)";
                break;
            }
            esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            if (err != ESP_OK) {
                err_msg = "esp_ota_begin failed";
                break;
            }
            began = true;
        }

        if (esp_ota_write(handle, buf, (size_t)r) != ESP_OK) {
            err_msg = "Flash write failed";
            break;
        }
        received += r;
        remaining -= r;
    }
    free(buf);

    if (err_msg) {
        if (began) esp_ota_abort(handle);
        ESP_LOGE(TAG, "Browser OTA aborted (%d/%d): %s", received, total, err_msg);
        char resp[160];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"msg\":\"%s\"}", err_msg);
        return send_json(req, "400 Bad Request", resp);
    }

    if (esp_ota_end(handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end verification failed");
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"msg\":\"Firmware verification failed\"}");
    }

    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"msg\":\"Failed to set boot partition\"}");
    }

    ota_updater_set_last_msg("Updated via Browser OTA!");
    ESP_LOGI(TAG, "Browser OTA OK (%d bytes -> %s), rebooting...", received, part->label);
    esp_err_t rc = send_json(req, "200 OK", "{\"ok\":true,\"msg\":\"Flashed! Rebooting...\"}");
    schedule_reboot(false);
    return rc;
}

static esp_err_t post_api_ota_sta(httpd_req_t *req) {
    if (!ota_updater_schedule_sta_ota()) {
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"msg\":\"Save your Home Wi-Fi SSID first\"}");
    }
    esp_err_t rc = send_json(req, "200 OK",
                             "{\"ok\":true,\"msg\":\"Rebooting into Home Wi-Fi Auto-OTA...\"}");
    schedule_reboot(false);
    return rc;
}

static int parse_slot_query(httpd_req_t *req) {
    char q[32] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char sl[8] = {0};
        if (httpd_query_key_value(q, "slot", sl, sizeof(sl)) == ESP_OK) {
            int s = atoi(sl);
            if (s >= 0 && s < USB_MAX_PLAYERS) return s;
        }
    }
    return 0;
}

static esp_err_t get_api_input(httpd_req_t *req) {
    int slot = parse_slot_query(req);
    uint32_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t lt = 0, rt = 0;
    usb_gamepad_get_latest_input_slot(slot, &b, &lx, &ly, &rx, &ry, &lt, &rt);
    int rssi = wifi_ap_get_peer_rssi(stadia_transport_get_slot_peer_ip(slot), get_req_client_ip4(req));
    int batt = stadia_transport_get_slot_battery_pct(slot);
    char buf[180];
    snprintf(buf, sizeof(buf),
             "{\"slot\":%d,\"ready\":%s,\"rssi\":%d,\"batt\":%d,\"b\":%lu,\"lx\":%ld,\"ly\":%ld,\"rx\":%ld,\"ry\":%ld,\"lt\":%u,\"rt\":%u}",
             slot,
             stadia_transport_is_slot_ready(slot) ? "true" : "false",
             rssi, batt,
             (unsigned long)b, (long)lx, (long)ly, (long)rx, (long)ry,
             (unsigned)lt, (unsigned)rt);
    return send_json(req, "200 OK", buf);
}

static int s_last_rmb_slot = 0;
static void rumble_off_cb(void *arg) {
    (void)arg;
    stadia_transport_set_rumble_slot(s_last_rmb_slot, 0, 0);
}

static esp_err_t post_api_rumble(httpd_req_t *req) {
    int slot = parse_slot_query(req);
    s_last_rmb_slot = slot;
    static esp_timer_handle_t s_rmb_tmr = NULL;
    if (!s_rmb_tmr) {
        const esp_timer_create_args_t targs = {
            .callback = &rumble_off_cb,
            .name = "web_rmb",
        };
        (void)esp_timer_create(&targs, &s_rmb_tmr);
    }
    if (s_rmb_tmr) (void)esp_timer_stop(s_rmb_tmr);
    stadia_transport_set_rumble_slot(slot, 0xC000, 0xC000);
    if (s_rmb_tmr) (void)esp_timer_start_once(s_rmb_tmr, 250000);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t post_api_ble_prov(httpd_req_t *req) {
    ble_provision_trigger_now();
    return send_json(req, "200 OK", "{\"ok\":true,\"msg\":\"Scanning for Stadia controller in orange-blink mode...\"}");
}

static esp_err_t post_api_reboot(httpd_req_t *req) {
    char q[64] = {0};
    bool to_boot = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char mode[24] = {0};
        if (httpd_query_key_value(q, "mode", mode, sizeof(mode)) == ESP_OK &&
            strcmp(mode, "bootloader") == 0) {
            to_boot = true;
        }
    }
    esp_err_t rc = send_json(req, "200 OK",
                             to_boot ? "{\"ok\":true,\"msg\":\"Entering USB Bootloader...\"}"
                                     : "{\"ok\":true,\"msg\":\"Rebooting...\"}");
    schedule_reboot(to_boot);
    return rc;
}

void web_ui_register(httpd_handle_t server) {
    static const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = get_root },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = get_api_status },
        { .uri = "/api/input",    .method = HTTP_GET,  .handler = get_api_input },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = post_api_settings },
        { .uri = "/api/ota",      .method = HTTP_POST, .handler = post_api_ota },
        { .uri = "/api/ota-sta",  .method = HTTP_POST, .handler = post_api_ota_sta },
        { .uri = "/api/ble-prov", .method = HTTP_POST, .handler = post_api_ble_prov },
        { .uri = "/api/rumble",   .method = HTTP_POST, .handler = post_api_rumble },
        { .uri = "/api/reboot",   .method = HTTP_POST, .handler = post_api_reboot },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "Web UI + OTA ready at http://192.168.4.1/");
}
