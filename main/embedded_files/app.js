(function(){
  var statusTimer=null;
  var authHeader='';
  var NET_MODE_REPEATER=0;
  var NET_MODE_TS_GATEWAY=1;
  var schedRules=[];
  var schedMode=0;

  function getAuthHeaders(){return {'Authorization':authHeader}}
  function authFetch(url,opts){
    opts=opts||{};
    opts.headers=Object.assign(getAuthHeaders(),opts.headers||{});
    return fetch(url,opts);
  }

  // Login
  function showLogin(){
    document.getElementById('loginOverlay').style.display='flex';
    document.getElementById('loginUser').focus();
  }
  function hideLogin(){document.getElementById('loginOverlay').style.display='none'}

  window.doLogin=function(){
    var u=document.getElementById('loginUser').value.trim();
    var p=document.getElementById('loginPass').value;
    if(!u||!p){document.getElementById('loginError').style.display='block';return}
    authHeader='Basic '+btoa(u+':'+p);
    fetch('/api/auth/check',{headers:getAuthHeaders()}).then(function(r){
      if(r.ok){
        hideLogin();
        document.getElementById('loginError').style.display='none';
        sessionStorage.setItem('auth',authHeader);
        startApp();
      }else{
        document.getElementById('loginError').style.display='block';
        authHeader='';
      }
    }).catch(function(){document.getElementById('loginError').style.display='block';authHeader=''});
  };

  document.getElementById('loginPass').addEventListener('keydown',function(e){if(e.key==='Enter')doLogin()});

  // Tab navigation
  document.querySelectorAll('.tab').forEach(function(tab){
    tab.addEventListener('click',function(){
      document.querySelectorAll('.tab').forEach(function(t){t.classList.remove('active')});
      document.querySelectorAll('.panel').forEach(function(p){p.classList.remove('active')});
      tab.classList.add('active');
      document.getElementById(tab.dataset.tab).classList.add('active');
      if(tab.dataset.tab==='clients') loadClients();
      if(tab.dataset.tab==='logs') loadLogs();
      if(tab.dataset.tab==='scheduler') loadScheduler();
    });
  });

  document.getElementById('eapEnabled').addEventListener('change',function(){
    document.getElementById('eapFields').style.display=this.checked?'block':'none';
  });

  document.getElementById('tsExposeLan').addEventListener('change',function(){
    document.getElementById('tsCidrGroup').style.display=this.checked?'block':'none';
    document.getElementById('tsSubnetMode').value=this.checked?NET_MODE_TS_GATEWAY:NET_MODE_REPEATER;
    if(this.checked){
      var inp=document.getElementById('tsAdvertiseCidr');
      if(!inp.value || inp.value==='192.168.24.0/24'){
        var derived=deriveStaCidr();
        if(derived) inp.value=derived;
      }
    }
    updateSubnetModeUI();
  });

  document.getElementById('tsSubnetMode').addEventListener('change',function(){
    updateSubnetModeUI();
  });

  // Last seen STA IP from /api/status (used to default the advertise CIDR)
  var lastStaIp='';
  var lastStaConnected=false;
  function deriveStaCidr(){
    if(!lastStaConnected||!lastStaIp) return '';
    var p=lastStaIp.split('.');
    if(p.length!==4) return '';
    return p[0]+'.'+p[1]+'.'+p[2]+'.0/24';
  }

  function getSubnetMode(){
    return document.getElementById('tsExposeLan').checked?NET_MODE_TS_GATEWAY:NET_MODE_REPEATER;
  }

  function updateSubnetModeUI(){
    var expose=document.getElementById('tsExposeLan').checked;
    var mode=getSubnetMode();
    var gatewayWarning=document.getElementById('tsGatewayWarning');
    var modeGroup=document.getElementById('tsSubnetModeGroup');

    document.getElementById('tsSubnetMode').value=mode;
    modeGroup.style.display='block';
    gatewayWarning.style.display=expose&&mode===NET_MODE_TS_GATEWAY?'block':'none';
    updatePortForwardingMode(mode);
  }

  function updatePortForwardingMode(mode){
    var gateway=mode===NET_MODE_TS_GATEWAY;
    var section=document.getElementById('portFwdSection');
    var warning=document.getElementById('pfModeWarning');
    var add=document.getElementById('btnAddPort');
    section.classList.toggle('pf-disabled',gateway);
    warning.style.display=gateway?'block':'none';
    add.disabled=gateway;
    document.querySelectorAll('#portFwdList input,#portFwdList select,#portFwdList button').forEach(function(el){
      el.disabled=gateway;
    });
  }

  function toast(msg,type){
    var t=document.getElementById('toast');
    t.textContent=msg;
    t.className='toast '+(type||'')+' show';
    setTimeout(function(){t.classList.remove('show')},3000);
  }

  // Modal system
  function showModal(title, body, onConfirm, okText, danger){
    var overlay = document.getElementById('modalOverlay');
    var btnOk = document.getElementById('modalBtnOk');
    var btnCancel = document.getElementById('modalBtnCancel');
    document.getElementById('modalTitle').textContent = title;
    document.getElementById('modalBody').innerHTML = body;
    btnOk.textContent = okText || 'Confirm';
    btnOk.className = 'btn ' + (danger ? 'btn-danger' : 'btn-primary') + ' btn-sm';
    
    var close = function(){ overlay.style.display = 'none'; };
    btnCancel.onclick = close;
    btnOk.onclick = function(){ close(); if(onConfirm) onConfirm(); };
    overlay.style.display = 'flex';
  }

  function rssiToPercent(rssi){
    if(rssi>=-50) return 100;
    if(rssi<=-100) return 0;
    return 2*(rssi+100);
  }

  function rssiColor(rssi){
    if(rssi>=-60) return 'var(--green)';
    if(rssi>=-75) return 'var(--orange)';
    return 'var(--red)';
  }

  function authName(a){
    var names=['Open','WEP','WPA','WPA2','WPA/2','WPA2-E','WPA3','WPA2/3','WAPI','OWE'];
    return names[a]||'?';
  }

  function formatUptime(s){
    var d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60);
    if(d>0) return d+'d '+h+'h '+m+'m';
    if(h>0) return h+'h '+m+'m';
    return m+'m '+Math.floor(s%60)+'s';
  }

  function updateWifiControl(d){
    var pill=document.getElementById('wifiStatePill');
    var meta=document.getElementById('wifiStateMeta');
    var note=document.getElementById('wifiStateNote');
    var pause=document.getElementById('btnWifiPause');
    var resume=document.getElementById('btnWifiResume');
    if(!pill||!meta) return;

    pill.classList.remove('connected','recovery','paused','pending');
    if(d.sta_paused){
      pill.textContent='STA paused';
      pill.classList.add('paused');
      if(note) note.textContent='Upstream is stopped; AP remains available at 192.168.4.1';
    }else if(d.sta_recovery){
      pill.textContent='Recovery mode';
      pill.classList.add('recovery');
      if(note) note.textContent='Captive portal remains available while STA backs off';
    }else if(d.sta_connected){
      pill.textContent='STA connected';
      pill.classList.add('connected');
      if(note) note.textContent='Pause stops upstream WiFi and Tailscale until resumed';
    }else{
      pill.textContent='STA reconnecting';
      if(note) note.textContent='AP remains available at 192.168.4.1';
    }

    var retry=d.sta_retry_count!==undefined?d.sta_retry_count:0;
    var next=d.sta_next_retry_s||0;
    meta.textContent=next>0?'Retry '+retry+' · next in '+next+'s':'Retry '+retry;
    if(pause) pause.disabled=!!d.sta_paused;
    if(resume) resume.disabled=!d.sta_paused;
  }

  function setWifiPending(label,metaText){
    var pill=document.getElementById('wifiStatePill');
    var meta=document.getElementById('wifiStateMeta');
    var note=document.getElementById('wifiStateNote');
    var pause=document.getElementById('btnWifiPause');
    var resume=document.getElementById('btnWifiResume');
    if(pill){
      pill.classList.remove('connected','recovery','paused');
      pill.classList.add('pending');
      pill.textContent=label;
    }
    if(meta) meta.textContent=metaText;
    if(note) note.textContent='If this page drops, connect to the AP and open 192.168.4.1';
    if(pause) pause.disabled=true;
    if(resume) resume.disabled=true;
  }

  function updateStatus(){
    authFetch('/api/status').then(function(r){
      if(r.status===401){showLogin();return}
      return r.json();
    }).then(function(d){
      if(!d) return;
      var dot=document.getElementById('statusDot');
      if(d.sta_connected){dot.classList.add('connected');dot.title='Connected'}
      else{dot.classList.remove('connected');dot.title='Disconnected'}
      lastStaConnected=!!d.sta_connected;
      lastStaIp=d.sta_ip||'';
      updateSubnetModeUI();
      document.getElementById('staSSID').textContent=d.sta_ssid||'Not set';
      document.getElementById('staIP').textContent=d.sta_connected?d.sta_ip:'Disconnected';
      document.getElementById('staRSSI').textContent=d.sta_connected?d.sta_rssi+' dBm':'-- dBm';
      var pct=d.sta_connected?rssiToPercent(d.sta_rssi):0;
      var fill=document.getElementById('rssiFill');
      fill.style.width=pct+'%';
      fill.style.background=d.sta_connected?rssiColor(d.sta_rssi):'var(--surface2)';
      document.getElementById('apClients').textContent=d.ap_clients;
      document.getElementById('apIP').textContent=d.ap_ip;
      document.getElementById('staMAC').textContent='STA: '+(d.sta_mac||'--');
      document.getElementById('apMAC').textContent='AP: '+(d.ap_mac||'--');
      document.getElementById('freeHeap').textContent=Math.round(d.free_heap/1024)+' KB';
      document.getElementById('uptime').textContent='Uptime: '+formatUptime(d.uptime);
      updateWifiControl(d);
    }).catch(function(){});
  }

  window.pauseWifiSta=function(){
    var viaAp=location.hostname==='192.168.4.1';
    var run = function(){
      setWifiPending('Pausing STA...','Stopping upstream WiFi');
      authFetch('/api/wifi/pause',{method:'POST'}).then(function(r){return r.json()}).then(function(d){
        toast(d.message||'STA paused','success');
        setTimeout(updateStatus,1200);
      }).catch(function(){
        toast('Pause sent; reconnect via AP if this page stops updating','success');
      });
    };
    if(viaAp) { run(); }
    else {
      showModal('Pause Upstream WiFi', 'Pausing STA will disconnect upstream WiFi. If this page drops, you must connect to the ESP32 AP and open http://192.168.4.1. Continue?', run, 'Pause STA', true);
    }
  };

  window.resumeWifiSta=function(){
    setWifiPending('Resuming STA...','Scheduling upstream reconnect');
    authFetch('/api/wifi/resume',{method:'POST'}).then(function(r){return r.json()}).then(function(d){
      toast(d.message||'STA resumed','success');
      setTimeout(updateStatus,1200);
    }).catch(function(){toast('Resume failed','error')});
  };

  function setTailscaleStatusUI(d){
    var state='not_available';
    var ip='--';
    var route='--';
    var routeState='not_advertised';
    var cls='';
    if(d&&d.available){
      state=d.state||'disabled';
      ip=d.vpn_ip||'--';
      route=d.subnet_route_advertised?(d.advertised_cidr||'--'):'--';
      routeState=d.subnet_route_state||'unknown';
      if(d.connected){cls='connected'}
      else if(state==='error'){cls='error'}
    }else if(d&&d.reason){
      state=d.reason;
      routeState=d.reason;
    }

    document.getElementById('tsState').textContent=state;
    document.getElementById('tsIP').textContent=ip;
    document.getElementById('tsConfigIP').textContent=ip;
    document.getElementById('tsStatusText').value=state;
    document.getElementById('tsAdvertisedRoute').value=route;
    document.getElementById('tsRouteState').value=routeState;
    var pill=document.getElementById('tsConfigState');
    pill.textContent=state;
    pill.className='ts-pill '+cls;
  }

  function updateTailscaleStatus(){
    authFetch('/api/tailscale/status').then(function(r){return r.json()}).then(function(d){
      setTailscaleStatusUI(d);
    }).catch(function(){});
  }

  function loadTailscaleConfig(){
    authFetch('/api/tailscale/config').then(function(r){return r.json()}).then(function(cfg){
      document.getElementById('tsEnabled').checked=!!cfg.enabled;
      document.getElementById('tsAuthKey').value='';
      document.getElementById('tsAuthKeyState').textContent=cfg.auth_key_set?'Key stored':'No key stored';
      document.getElementById('tsDeviceName').value=cfg.device_name||'esp32-repeater';
      document.getElementById('tsControlHost').value=cfg.control_host||'';
      document.getElementById('tsDerp').checked=cfg.enable_derp!==false;
      document.getElementById('tsDisco').checked=cfg.enable_disco!==false;
      document.getElementById('tsStun').checked=cfg.enable_stun!==false;
      document.getElementById('tsMaxPeers').value=cfg.max_peers||8;
      var exposeLan=!!cfg.expose_lan;
      document.getElementById('tsExposeLan').checked=exposeLan;
      document.getElementById('tsCidrGroup').style.display=exposeLan?'block':'none';
      document.getElementById('tsSubnetMode').value=exposeLan?NET_MODE_TS_GATEWAY:NET_MODE_REPEATER;
      var savedCidr=cfg.advertise_cidr||'';
      // Si no hay valor guardado o es el default histórico "192.168.24.0/24",
      // y la STA está conectada al AP padre, sugerir su red /24 como default.
      var derived=deriveStaCidr();
      if((!savedCidr || savedCidr==='192.168.24.0/24') && derived){
        document.getElementById('tsAdvertiseCidr').value=derived;
      }else{
        document.getElementById('tsAdvertiseCidr').value=savedCidr;
      }
      document.getElementById('tsAdvertisedRoute').value=exposeLan?(savedCidr||'--'):'--';
      updateSubnetModeUI();
    }).catch(function(){});
  }

  window.saveTailscaleConfig=function(){
    var maxPeers=parseInt(document.getElementById('tsMaxPeers').value)||8;
    if(maxPeers<1||maxPeers>64){toast('Max peers must be 1-64','error');return}
    var name=document.getElementById('tsDeviceName').value.trim()||'esp32-repeater';
    var key=document.getElementById('tsAuthKey').value;
    var mode=getSubnetMode();

    var runSave = function(){
      var data={
        enabled:document.getElementById('tsEnabled').checked,
        device_name:name,
        control_host:document.getElementById('tsControlHost').value.trim(),
        enable_derp:document.getElementById('tsDerp').checked,
        enable_disco:document.getElementById('tsDisco').checked,
        enable_stun:document.getElementById('tsStun').checked,
        max_peers:maxPeers,
        expose_lan:document.getElementById('tsExposeLan').checked,
        net_mode:mode,
        advertise_cidr:document.getElementById('tsAdvertiseCidr').value.trim()||deriveStaCidr()||'192.168.24.0/24'
      };
      if(key.length>0) data.auth_key=key;

      authFetch('/api/tailscale/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
      .then(function(r){
        if(!r.ok) throw new Error('save failed');
        return r.json();
      }).then(function(d){
        document.getElementById('tsAuthKey').value='';
        toast(d.message||'Tailscale saved','success');
        loadTailscaleConfig();
        updateTailscaleStatus();
      }).catch(function(){toast('Tailscale save failed','error')});
    };

    if(mode===NET_MODE_TS_GATEWAY){
      showModal('Switch to Gateway Mode', 'Tailscale Gateway / SNAT disables the usable repeater path (AP NAPT and port forwarding). The device will reboot to apply NAT changes. Continue?', runSave, 'Apply & Reboot', true);
    } else {
      runSave();
    }
  };

  window.clearTailscaleKey=function(){
    showModal('Clear Tailscale Key', 'Are you sure you want to clear the stored Tailscale auth key? Tailscale will be disabled.', function(){
      var data={
        auth_key:'',
        enabled:false
      };
      authFetch('/api/tailscale/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
      .then(function(r){
        if(!r.ok) throw new Error('clear failed');
        return r.json();
      }).then(function(){
        document.getElementById('tsAuthKey').value='';
        document.getElementById('tsEnabled').checked=false;
        toast('Tailscale key cleared','success');
        loadTailscaleConfig();
        updateTailscaleStatus();
      }).catch(function(){toast('Clear failed','error')});
    }, 'Clear Key', true);
  };

  window.doScan=function(){
    var btn=document.getElementById('btnScan');
    btn.disabled=true;
    btn.innerHTML='<span class="spinner"></span> Scanning...';
    var list=document.getElementById('scanResults');
    list.style.display='none';
    list.innerHTML='';

    authFetch('/api/scan').then(function(r){return r.json()}).then(function(aps){
      btn.disabled=false;
      btn.innerHTML='<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/></svg> Scan Networks';
      if(!aps.length){toast('No networks found','error');return}
      list.style.display='block';
      aps.forEach(function(ap){
        var div=document.createElement('div');
        div.className='scan-item';
        div.innerHTML='<div class="scan-item-info"><span class="scan-item-ssid">'+(ap.ssid||'[Hidden]')+'</span><span class="scan-item-meta">CH '+ap.channel+' · '+authName(ap.auth)+'</span></div><span class="scan-item-rssi" style="color:'+rssiColor(ap.rssi)+'">'+ap.rssi+' dBm</span>';
        div.addEventListener('click',function(){
          document.getElementById('staSSIDInput').value=ap.ssid;
          document.getElementById('staPassInput').focus();
          list.style.display='none';
          toast('Selected: '+ap.ssid,'success');
        });
        list.appendChild(div);
      });
    }).catch(function(){
      btn.disabled=false;
      btn.innerHTML='<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/></svg> Scan Networks';
      toast('Scan failed','error');
    });
  };

  window.saveConfig=function(){
    var data={
      sta_ssid:document.getElementById('staSSIDInput').value,
      sta_pass:document.getElementById('staPassInput').value,
      ap_ssid:document.getElementById('apSSIDInput').value,
      ap_pass:document.getElementById('apPassInput').value,
      ap_channel:parseInt(document.getElementById('apChannel').value),
      ap_max_conn:parseInt(document.getElementById('apMaxConn').value),
      ap_hide_ssid:document.getElementById('apHideSSID').checked?1:0,
      sta_eap_enabled:document.getElementById('eapEnabled').checked?1:0,
      sta_eap_identity:document.getElementById('eapIdentity').value,
      sta_eap_username:document.getElementById('eapUser').value,
      sta_eap_password:document.getElementById('eapPass').value,
      sta_retry_max:parseInt(document.getElementById('staRetryMax').value,10)||5,
      sta_backoff_s:parseInt(document.getElementById('staBackoffS').value,10)||30,
      hostname:document.getElementById('hostnameInput').value,
      sta_mac_custom:document.getElementById('staMacCustom').checked,
      sta_mac:document.getElementById('staMacInput').value.trim(),
      ap_mac_custom:document.getElementById('apMacCustom').checked,
      ap_mac:document.getElementById('apMacInput').value.trim()
    };
    // Add port forwarding rules as flat keys
    var rules=document.querySelectorAll('.pf-rule');
    rules.forEach(function(rule,i){
      data['pf'+i+'_enabled']=rule.querySelector('.pf-enabled').checked?1:0;
      data['pf'+i+'_proto']=parseInt(rule.querySelector('.pf-proto').value);
      data['pf'+i+'_ext_port']=parseInt(rule.querySelector('.pf-ext-port').value)||0;
      data['pf'+i+'_int_ip']=rule.querySelector('.pf-int-ip').value;
      data['pf'+i+'_int_port']=parseInt(rule.querySelector('.pf-int-port').value)||0;
    });
    authFetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
    .then(function(r){return r.json()}).then(function(d){
      toast(d.message||'Config saved!','success');
    }).catch(function(){toast('Save failed','error')});
  };

  window.restartDevice=function(){
    showModal('Restart Device', 'Are you sure you want to restart the device?', function(){
      authFetch('/api/restart',{method:'POST'}).then(function(){
        toast('Restarting...','success');
      }).catch(function(){toast('Restart failed','error')});
    }, 'Restart');
  };

  window.loadClients=function(){
    authFetch('/api/clients').then(function(r){return r.json()}).then(function(clients){
      var list=document.getElementById('clientList');
      if(!clients.length){list.innerHTML='<div class="empty-state">No clients connected</div>';return}
      list.innerHTML='';
      clients.forEach(function(c){
        var div=document.createElement('div');
        div.className='client-item';
        div.innerHTML='<div class="client-dot"></div><div class="client-info"><span class="client-mac">'+c.mac+'</span><span class="client-ip">'+c.ip+'</span></div>';
        list.appendChild(div);
      });
    }).catch(function(){});
  };

  window.doPing=function(){
    var target=document.getElementById('pingTarget').value.trim();
    if(!target){toast('Enter a target','error');return}
    var btn=document.getElementById('btnPing');
    var res=document.getElementById('pingResult');
    btn.disabled=true;
    btn.textContent='Pinging...';
    res.className='ping-result ping-wait';
    res.textContent='Sending ping to '+target+'...';

    authFetch('/api/ping',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({target:target})})
    .then(function(r){return r.json()}).then(function(d){
      btn.disabled=false;
      btn.textContent='Ping';
      if(d.success){
        res.className='ping-result ping-ok';
        res.textContent='✓ Reply from '+d.ip+' time='+d.time_ms+'ms';
      }else{
        res.className='ping-result ping-fail';
        res.textContent='✗ '+(d.reason==='dns_failed'?'DNS lookup failed for '+d.target:'Request timed out');
      }
    }).catch(function(){
      btn.disabled=false;
      btn.textContent='Ping';
      res.className='ping-result ping-fail';
      res.textContent='✗ Request failed';
    });
  };

  window.togglePass=function(id){
    var inp=document.getElementById(id);
    inp.type=inp.type==='password'?'text':'password';
  };

  function applyLogLevelsRequest(lg, lm){
    var btn=document.getElementById('btnApplyLogLevels');
    var origText=btn?btn.textContent:'';
    if(btn){ btn.disabled=true; btn.textContent='Applying...'; }
    authFetch('/api/loglevel',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({log_level_global:lg,log_level_microlink:lm})
    }).then(function(r){
      if(!r.ok) throw new Error('http '+r.status);
      return r.json();
    }).then(function(d){
      toast(d.message||'Log levels applied','success');
      if(btn){
        btn.textContent='Applied \u2713';
        setTimeout(function(){ btn.disabled=false; btn.textContent=origText||'Apply log levels'; },1500);
      }
      // Refresh selects from server to reflect persisted state
      authFetch('/api/config').then(function(r){return r.json()}).then(function(cfg){
        var lge=document.getElementById('logLevelGlobal');
        var lme=document.getElementById('logLevelMicrolink');
        if(lge && cfg.log_level_global!==undefined) lge.value=String(cfg.log_level_global);
        if(lme && cfg.log_level_microlink!==undefined) lme.value=String(cfg.log_level_microlink);
      }).catch(function(){});
    }).catch(function(){
      toast('Failed to apply log levels','error');
      if(btn){ btn.disabled=false; btn.textContent=origText||'Apply log levels'; }
    });
  }

  window.saveLogLevels=function(){
    var lg=parseInt(document.getElementById('logLevelGlobal').value,10);
    var lm=parseInt(document.getElementById('logLevelMicrolink').value,10);
    if(isNaN(lg)||isNaN(lm)){toast('Invalid log level','error');return}
    if(lg===0){
      showModal(
        'Silence all system logs?',
        'Setting <strong>System log level</strong> to <strong>None</strong> will hide every diagnostic message, including errors. The Logs tab will stop showing new entries until you raise the level again. Continue?',
        function(){ applyLogLevelsRequest(lg, lm); },
        'Silence logs',
        true
      );
      return;
    }
    applyLogLevelsRequest(lg, lm);
  };

  window.factoryReset=function(){
    showModal('⚠️ Factory Reset', 'All settings will be erased (WiFi, AP, Credentials, Tailscale). The device will reboot with defaults. This cannot be undone.', function(){
      showModal('Confirm Factory Reset', 'Are you absolutely sure?', function(){
        authFetch('/api/factory-reset',{method:'POST'}).then(function(r){return r.json()}).then(function(d){
          toast(d.message||'Factory reset done!','success');
          setTimeout(function(){sessionStorage.removeItem('auth');location.reload()},3000);
        }).catch(function(){toast('Factory reset failed','error')});
      }, 'Reset Now', true);
    }, 'Factory Reset', true);
  };

  window.changeCredentials=function(){
    var u=document.getElementById('newUser').value.trim();
    var p=document.getElementById('newPass').value;
    if(!u){toast('Username required','error');return}
    if(p.length<4){toast('Password min 4 characters','error');return}
    authFetch('/api/auth/change',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({new_user:u,new_pass:p})})
    .then(function(r){return r.json()}).then(function(d){
      if(d.status==='ok'){
        authHeader='Basic '+btoa(u+':'+p);
        sessionStorage.setItem('auth',authHeader);
        toast('Credentials updated!','success');
        document.getElementById('newPass').value='';
      }else{
        toast(d.message||'Failed','error');
      }
    }).catch(function(){toast('Failed to update credentials','error')});
  };

  window.addPortRule=function(cfg){
    if(getSubnetMode()===NET_MODE_TS_GATEWAY){
      toast('Port forwarding is unavailable in Tailscale Gateway mode','error');
      return;
    }
    var list=document.getElementById('portFwdList');
    if(list.querySelectorAll('.pf-rule').length>=5){toast('Max 5 rules','error');return}
    cfg=cfg||{enabled:true,proto:0,ext_port:'',int_ip:'',int_port:''};
    var div=document.createElement('div');
    div.className='pf-rule';
    div.innerHTML='<div class="pf-row">'+
      '<input type="checkbox" class="pf-enabled"'+(cfg.enabled?' checked':'')+'>'+
      '<select class="pf-proto"><option value="0"'+(cfg.proto==0?' selected':'')+'>TCP</option><option value="1"'+(cfg.proto==1?' selected':'')+'>UDP</option></select>'+
      '<input type="number" class="pf-ext-port" placeholder="Ext" min="1" max="65535" value="'+(cfg.ext_port||'')+'">'+
      '<span class="pf-arrow">→</span>'+
      '<input type="text" class="pf-int-ip" placeholder="192.168.4.x" value="'+(cfg.int_ip||'')+'">'+
      '<input type="number" class="pf-int-port" placeholder="Int" min="1" max="65535" value="'+(cfg.int_port||'')+'">'+
      '<button class="pf-del" onclick="this.closest(\'.pf-rule\').remove()">✕</button>'+
      '</div>';
    list.appendChild(div);
    updateSubnetModeUI();
  };

  var logTimer=null;
  window.loadLogs=function(){
    authFetch('/api/logs').then(function(r){return r.text()}).then(function(txt){
      var el=document.getElementById('logContent');
      el.textContent=txt||'(empty)';
      el.scrollTop=el.scrollHeight;
    }).catch(function(){});
  };
  function startLogRefresh(){
    if(logTimer) clearInterval(logTimer);
    logTimer=setInterval(function(){
      if(document.getElementById('logAutoRefresh').checked &&
         document.getElementById('logs').classList.contains('active')){
        loadLogs();
      }
    },3000);
  }

  window.doOTA=function(){
    var fileInput=document.getElementById('otaFile');
    if(!fileInput.files.length){toast('Select a firmware file','error');return}
    var file=fileInput.files[0];
    
    showModal('Firmware Update', 'Upload <strong>'+file.name+'</strong> ('+Math.round(file.size/1024)+' KB)? The device will reboot after the update.', function(){
      var btn=document.getElementById('btnOta');
      var prog=document.getElementById('otaProgress');
      var fill=document.getElementById('otaFill');
      var status=document.getElementById('otaStatus');
      btn.disabled=true;
      prog.style.display='block';
      fill.style.width='0%';
      status.textContent='Uploading...';

      var xhr=new XMLHttpRequest();
      xhr.open('POST','/api/ota',true);
      xhr.setRequestHeader('Authorization',authHeader);
      xhr.setRequestHeader('Content-Type','application/octet-stream');

      xhr.upload.onprogress=function(e){
        if(e.lengthComputable){
          var pct=Math.round(e.loaded/e.total*100);
          fill.style.width=pct+'%';
          status.textContent='Uploading... '+pct+'%';
        }
      };

      xhr.onload=function(){
        if(xhr.status===200){
          fill.style.width='100%';
          fill.style.background='var(--green)';
          status.textContent='OTA successful! Rebooting...';
          toast('Firmware updated, rebooting...','success');
        }else{
          fill.style.background='var(--red)';
          status.textContent='OTA failed: '+xhr.responseText;
          toast('OTA failed','error');
          btn.disabled=false;
        }
      };

      xhr.onerror=function(){
        fill.style.background='var(--red)';
        status.textContent='Upload failed';
        toast('OTA upload failed','error');
        btn.disabled=false;
      };

      xhr.send(file);
    }, 'Update');
  };

  function minToTime(min){
    var h=Math.floor((min||0)/60),m=(min||0)%60;
    return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0');
  }

  function timeToMin(v){
    var p=(v||'00:00').split(':');
    return (parseInt(p[0],10)||0)*60+(parseInt(p[1],10)||0);
  }

  function escHtml(v){
    return String(v||'').replace(/[&<>"']/g,function(c){
      return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];
    });
  }

  function schedModeLabel(mode){
    return mode===1?'Scheduled':(mode===2?'Manual off':'Always on');
  }

  function schedDaysLabel(mask){
    if(mask===0x1f) return 'Mon-Fri';
    if(mask===0x60) return 'Weekend';
    if(mask===0x7f) return 'Every day';
    var days=['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];
    var out=[];
    days.forEach(function(d,i){if(mask&(1<<i)) out.push(d)});
    return out.length?out.join(' '):'No days';
  }

  function schedulerRuleSummary(r){
    var states=[
      '<span class="sched-pill '+(r.ap_enabled?'on':'off')+'">AP '+(r.ap_enabled?'ON':'OFF')+'</span>',
      '<span class="sched-pill '+(r.sta_enabled!==false?'on':'off')+'">STA '+(r.sta_enabled!==false?'ON':'OFF')+'</span>',
      '<span class="sched-pill '+(r.tailscale_enabled!==false?'on':'off')+'">TS '+(r.tailscale_enabled!==false?'ON':'OFF')+'</span>'
    ];
    return '<div class="sched-rule-tags">' +
      '<span class="sched-tag-days">' + schedDaysLabel(r.days) + '</span>' +
      '<span class="sched-tag-time">' + minToTime(r.start_min) + ' - ' + minToTime(r.end_min||0) + '</span>' +
      '<div class="sched-tag-states">' + states.join('') + '</div>' +
      '</div>';
  }

  function renderSchedulerMode(){
    document.querySelectorAll('.seg-btn').forEach(function(b){
      b.classList.toggle('active',parseInt(b.dataset.mode,10)===schedMode);
    });
    var warn=document.getElementById('schedWarning');
    if(!warn) return;
    if(schedMode===2){
      warn.textContent='Manual off: AP radio is permanently disabled. STA and Tailscale remain connected if configured.';
      warn.style.display='block';
      warn.className='notice danger';
    }else if(schedMode===1){
      warn.textContent='Scheduled: AP/STA/TS states will follow the rules below. AP is kept ON if STA is offline.';
      warn.style.display='block';
      warn.className='notice warning';
    }else{
      warn.style.display='none';
    }
  }

  window.setSchedulerMode=function(mode){
    schedMode=mode;
    renderSchedulerMode();
  };

  function renderSchedulerRules(){
    var box=document.getElementById('schedRules');
    if(!box) return;
    box.innerHTML='';
    var days=['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];
    schedRules.forEach(function(r,i){
      var row=document.createElement('div');
      row.className='sched-rule '+(r.enabled?'':'rule-disabled');
      var dayHtml=days.map(function(d,idx){
        return '<button type="button" class="day-btn '+((r.days&(1<<idx))?'active':'')+'" data-day="'+idx+'">'+d+'</button>';
      }).join('');
      row.innerHTML=
        '<div class="sched-rule-head">'+
          '<label class="toggle-label"><input type="checkbox" class="rule-enabled" '+(r.enabled?'checked':'')+'> <span class="sched-rule-title">Rule '+(i+1)+'</span></label>'+
          '<button class="pf-del" type="button" title="Delete">×</button>'+
        '</div>'+
        '<div class="sched-rule-summary">'+schedulerRuleSummary(r)+'</div>'+
        '<div class="day-buttons">'+dayHtml+'</div>'+
        '<div class="sched-quick-days">'+
          '<button type="button" data-mask="31">Weekdays</button>'+
          '<button type="button" data-mask="96">Weekend</button>'+
          '<button type="button" data-mask="127">Every day</button>'+
        '</div>'+
        '<div class="sched-rule-grid">'+
          '<div class="form-group"><label>Start</label><input type="time" class="rule-start" value="'+minToTime(r.start_min)+'"></div>'+
          '<div class="form-group"><label>End</label><input type="time" class="rule-end" value="'+minToTime(r.end_min||60)+'"></div>'+
        '</div>'+
        '<div class="ts-options">'+
          '<label class="toggle-label toggle-sm"><input type="checkbox" class="rule-ap" '+(r.ap_enabled?'checked':'')+'> AP Enabled</label>'+
          '<label class="toggle-label toggle-sm"><input type="checkbox" class="rule-sta" '+(r.sta_enabled!==false?'checked':'')+'> STA Enabled</label>'+
          '<label class="toggle-label toggle-sm"><input type="checkbox" class="rule-ts" '+(r.tailscale_enabled!==false?'checked':'')+'> TS Enabled</label>'+
        '</div>'+
        '<input type="text" class="rule-note" maxlength="31" placeholder="Note (e.g. Work hours)" value="'+escHtml(r.note||'')+'">';
      row.querySelector('.pf-del').onclick=function(){schedRules.splice(i,1);renderSchedulerRules()};
      row.querySelectorAll('.day-btn').forEach(function(btn){
        btn.onclick=function(){
          var bit=1<<parseInt(btn.dataset.day,10);
          r.days=(r.days^bit)&0x7f;
          renderSchedulerRules();
        };
      });
      row.querySelectorAll('.sched-quick-days button').forEach(function(btn){
        btn.onclick=function(){
          r.days=parseInt(btn.dataset.mask,10);
          renderSchedulerRules();
        };
      });
      row.querySelector('.rule-enabled').onchange=function(){collectSchedulerRules();renderSchedulerRules()};
      row.querySelector('.rule-start').onchange=function(){collectSchedulerRules();renderSchedulerRules()};
      row.querySelector('.rule-end').onchange=function(){collectSchedulerRules();renderSchedulerRules()};
      row.querySelector('.rule-ap').onchange=function(){collectSchedulerRules();renderSchedulerRules()};
      row.querySelector('.rule-sta').onchange=function(){collectSchedulerRules();renderSchedulerRules()};
      row.querySelector('.rule-ts').onchange=function(){collectSchedulerRules();renderSchedulerRules()};
      row.querySelector('.rule-note').oninput=function(){r.note=this.value.trim()};
      box.appendChild(row);
    });
    if(schedRules.length===0){
      box.innerHTML='<div class="empty-state">No schedule rules</div>';
    }
  }

  function collectSchedulerRules(){
    var rows=document.querySelectorAll('.sched-rule');
    rows.forEach(function(row,i){
      if(!schedRules[i]) return;
      schedRules[i].enabled=row.querySelector('.rule-enabled').checked;
      schedRules[i].start_min=timeToMin(row.querySelector('.rule-start').value);
      schedRules[i].end_min=timeToMin(row.querySelector('.rule-end').value);
      schedRules[i].ap_enabled=row.querySelector('.rule-ap').checked;
      schedRules[i].sta_enabled=row.querySelector('.rule-sta').checked;
      schedRules[i].tailscale_enabled=row.querySelector('.rule-ts').checked;
      schedRules[i].note=row.querySelector('.rule-note').value.trim();
    });
  }

  window.addSchedulerRule=function(){
    if(schedRules.length>=8){toast('Maximum 8 rules','error');return}
    schedRules.push({enabled:true,days:1,start_min:14*60,end_min:16*60,ap_enabled:false,sta_enabled:true,tailscale_enabled:true,note:'Work access'});
    renderSchedulerRules();
  };

  window.addWorkAccessRule=function(){
    if(schedRules.length>=8){toast('Maximum 8 rules','error');return}
    schedRules.push({enabled:true,days:1,start_min:14*60,end_min:16*60,ap_enabled:false,sta_enabled:true,tailscale_enabled:true,note:'Work access'});
    schedMode=1;
    renderSchedulerMode();
    renderSchedulerRules();
  };

  window.clearSchedulerRules=function(){
    if(!schedRules.length) return;
    showModal('Clear Rules', 'Are you sure you want to clear all scheduler rules?', function(){
      schedRules=[];
      renderSchedulerRules();
    }, 'Clear All', true);
  };

  function loadSchedulerStatus(){
    authFetch('/api/scheduler/status').then(function(r){return r.json()}).then(function(s){
      var local=document.getElementById('schedLocalTime');
      var state=schedModeLabel(s.mode);
      var ap=(s.ap_effective?'On':'Off')+(s.safety_hold?' (held)':'');
      var sta=s.sta_effective?'On':'Off';
      var ts=s.tailscale_effective?'On':'Off';
      var next=s.next_change_local&&s.next_change_local!=='--'?s.next_change_local:'No scheduled change';
      var reason=s.reason||state;
      var dash=document.getElementById('schedDashState');
      if(dash){
        dash.textContent=state;
        var summary = (s.ap_effective?'AP ON':'AP OFF') + ' · ' + (s.sta_effective?'STA ON':'STA OFF');
        if (s.tailscale_effective !== undefined) summary += ' · ' + (s.tailscale_effective?'TS ON':'TS OFF');
        document.getElementById('schedDashNext').textContent=summary;
      }
      if(!local) return;
      local.textContent=s.local_time||'--';
      document.getElementById('schedNtpState').textContent=s.time_valid?(s.ntp_synced?'Synced':'Time valid'):'Waiting';
      document.getElementById('schedApState').textContent=ap;
      document.getElementById('schedStaState').textContent=sta;
      document.getElementById('schedTsState').textContent=ts;
      document.getElementById('schedNextChange').textContent=next;
      var primary=document.getElementById('schedPrimaryStatus');
      if(primary){
        primary.className='sched-primary-status '+(!s.ap_effective?'off':(s.safety_hold?'hold':''));
        primary.querySelector('strong').textContent=state+' · '+ap+' · '+sta+' · '+ts;
        primary.querySelector('span').textContent=reason+' · '+next;
      }
    }).catch(function(){});
  }

  function loadScheduler(){
    authFetch('/api/scheduler/config').then(function(r){return r.json()}).then(function(cfg){
      schedMode=cfg.mode||0;
      schedRules=(cfg.rules||[]).filter(function(r){return r.enabled||r.days||r.start_min||r.end_min||r.note});
      schedRules.forEach(function(r){
        if(r.sta_enabled===undefined) r.sta_enabled=true;
        if(r.tailscale_enabled===undefined) r.tailscale_enabled=true;
      });
      document.getElementById('schedTimezone').value=cfg.timezone||'WET0WEST,M3.5.0/1,M10.5.0';
      renderSchedulerMode();
      renderSchedulerRules();
      loadSchedulerStatus();
    }).catch(function(){});
  }

  window.saveScheduler=function(){
    collectSchedulerRules();
    for(var j=0;j<schedRules.length;j++){
      var vr=schedRules[j];
      if(vr.enabled){
        if(!vr.days){toast('Rule '+(j+1)+': select at least one day','error');return}
        if(vr.start_min>=vr.end_min){toast('Rule '+(j+1)+': end must be after start','error');return}
      }
    }
    var data={timezone:document.getElementById('schedTimezone').value,mode:schedMode};
    for(var i=0;i<8;i++){
      var r=schedRules[i]||{enabled:false,days:0,start_min:0,end_min:0,ap_enabled:true,sta_enabled:true,tailscale_enabled:true,note:''};
      data['rule'+i+'_enabled']=!!r.enabled;
      data['rule'+i+'_days']=r.days||0;
      data['rule'+i+'_start_min']=r.start_min||0;
      data['rule'+i+'_end_min']=r.end_min||0;
      data['rule'+i+'_ap_enabled']=!!r.ap_enabled;
      data['rule'+i+'_sta_enabled']=r.sta_enabled!==false;
      data['rule'+i+'_tailscale_enabled']=r.tailscale_enabled!==false;
      data['rule'+i+'_note']=r.note||'';
    }
    authFetch('/api/scheduler/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
    .then(function(r){if(!r.ok) throw new Error(); return r.json()}).then(function(d){
      toast(d.message||'Scheduler saved','success');
      setTimeout(loadSchedulerStatus,800);
    }).catch(function(){toast('Scheduler save failed','error')});
  };

  window.syncSchedulerTime=function(){
    authFetch('/api/scheduler/sync',{method:'POST'}).then(function(r){return r.json()}).then(function(d){
      toast(d.message||'Time sync requested','success');
      setTimeout(loadSchedulerStatus,1500);
    }).catch(function(){toast('Time sync failed','error')});
  };

  function startApp(){
    authFetch('/api/config').then(function(r){return r.json()}).then(function(cfg){
      document.getElementById('staSSIDInput').value=cfg.sta_ssid||'';
      document.getElementById('apSSIDInput').value=cfg.ap_ssid||'';
      document.getElementById('apChannel').value=cfg.ap_channel||0;
      document.getElementById('apMaxConn').value=cfg.ap_max_conn||4;
      document.getElementById('apHideSSID').checked=!!cfg.ap_hide_ssid;
      document.getElementById('staRetryMax').value=cfg.sta_retry_max||5;
      document.getElementById('staBackoffS').value=cfg.sta_backoff_s||30;
      document.getElementById('hostnameInput').value=cfg.hostname||'esp32-repeater';
      document.getElementById('staMacCustom').checked=!!cfg.sta_mac_custom;
      document.getElementById('staMacInput').value=cfg.sta_mac||'';
      document.getElementById('apMacCustom').checked=!!cfg.ap_mac_custom;
      document.getElementById('apMacInput').value=cfg.ap_mac||'';
      // EAP
      var eapCb=document.getElementById('eapEnabled');
      eapCb.checked=cfg.sta_eap_enabled||false;
      document.getElementById('eapFields').style.display=eapCb.checked?'block':'none';
      document.getElementById('eapIdentity').value=cfg.sta_eap_identity||'';
      document.getElementById('eapUser').value=cfg.sta_eap_username||'';
      // Port forwarding
      document.getElementById('portFwdList').innerHTML='';
      if(cfg.port_fwd){
        cfg.port_fwd.forEach(function(r){
          if(r.ext_port>0||r.enabled) addPortRule(r);
        });
      }
      // Log levels
      var lg=document.getElementById('logLevelGlobal');
      var lm=document.getElementById('logLevelMicrolink');
      if(lg) lg.value=String(cfg.log_level_global!==undefined?cfg.log_level_global:3);
      if(lm) lm.value=String(cfg.log_level_microlink!==undefined?cfg.log_level_microlink:2);
      updateSubnetModeUI();
    }).catch(function(){});
    loadTailscaleConfig();
    updateStatus();
    updateTailscaleStatus();
    loadScheduler();
    if(statusTimer) clearInterval(statusTimer);
    statusTimer=setInterval(updateStatus,3000);
    startLogRefresh();
  }

  // Check saved session or show login
  var saved=sessionStorage.getItem('auth');
  if(saved){
    authHeader=saved;
    fetch('/api/auth/check',{headers:{'Authorization':authHeader}}).then(function(r){
      if(r.ok){startApp()}
      else{sessionStorage.removeItem('auth');authHeader='';showLogin()}
    }).catch(function(){showLogin()});
  }else{
    showLogin();
  }
})();
