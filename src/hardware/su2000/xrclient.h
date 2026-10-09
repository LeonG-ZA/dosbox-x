/*
 *  WebXR page served by xrserver.cpp. Protocol: see xrserver.cpp.
 *
 *  Each eye's PIX image is drawn on a large quad placed with the head pose the frame was rendered for (yaw and pitch
 *  only), so head motion between emulator frames is corrected by the headset (rotational time-warp) and head roll
 *  keeps the horizon level. Real-time traffic uses a WebRTC data channel (unordered, no retransmits) when it opens,
 *  else the WebSocket. URL parameters: player=1|2, fov=<horizontal degrees>, aspect=<width/height>, rtc=0 (WebSocket
 *  only). (MSVC limits one string literal to 16 KB, hence the pieces.)
 */
#ifndef SU2000_XRCLIENT_H
#define SU2000_XRCLIENT_H

#include <string>

static inline std::string XR_ClientPage(void) {
    std::string s;
    s += R"XR(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SU2000 VR</title>
<style>
body{background:#111;color:#ddd;font:15px system-ui,sans-serif;margin:0;padding:16px}
h1{font-size:20px;margin:0 0 12px}
#bar{display:flex;flex-wrap:wrap;gap:12px;align-items:center;margin-bottom:12px}
button{font-size:16px;padding:8px 18px}
input,select{font-size:15px}
#prev{max-width:100%;background:#000}
#st{color:#8c8}
</style></head><body>
<h1>SU2000 VR</h1>
<div id="bar">
<label>Player <select id="pl"><option>1</option><option>2</option></select></label>
<label>FOV <input id="fov" type="number" min="20" max="140" value="60" style="width:4em">&deg;</label>
<label><input id="snd" type="checkbox" checked> Sound</label>
<label><input id="mic" type="checkbox"> Microphone</label>
<button id="vr" disabled>Enter VR</button>
</div>
<div id="st">connecting...</div>
<p><canvas id="prev" width="352" height="250"></canvas></p>
<p>Trigger = fire, grip or A/X = walk, B/Y = recentre, thumbstick up/down = field of view.
Sound and microphone start with the first click (browser rule).</p>
)XR";
    s += R"XR(<script>
'use strict';
const Q=new URLSearchParams(location.search);
const $=id=>document.getElementById(id);
let player=+(Q.get('player')||1), fov=+(Q.get('fov')||60), aspectQ=+(Q.get('aspect')||0);
const DIST=10;
$('pl').value=player; $('fov').value=fov;
$('pl').onchange=()=>{player=+$('pl').value; if(ws&&ws.readyState==1) ws.send('S '+player);};
$('fov').onchange=()=>{fov=+$('fov').value;};

/* ---- transport: WebSocket for control and signalling, WebRTC data channel for real-time data ---- */
let ws=null, pc=null, dc=null, dcOpen=false;
function connect(){
  ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws'); ws.binaryType='arraybuffer';
  ws.onopen=()=>{ws.send('S '+player); startRTC();};
  ws.onclose=()=>{dcOpen=false; if(pc){pc.close();pc=null;} setTimeout(connect,1000);};
  ws.onmessage=e=>{ if(typeof e.data==='string') signal(e.data); else binary(e.data); };
}
function startRTC(){
  if(!window.RTCPeerConnection||Q.get('rtc')==='0') return;
  pc=new RTCPeerConnection({iceServers:[]});
  dc=pc.createDataChannel('su',{ordered:false,maxRetransmits:0}); dc.binaryType='arraybuffer';
  dc.onopen=()=>{dcOpen=true;}; dc.onclose=()=>{dcOpen=false;}; dc.onmessage=e=>{ if(typeof e.data!=='string') binary(e.data); };
  pc.onicecandidate=e=>{ if(e.candidate&&e.candidate.candidate&&ws.readyState==1) ws.send('C '+e.candidate.candidate+'\n'+(e.candidate.sdpMid||'0')); };
  pc.createOffer().then(o=>pc.setLocalDescription(o)).then(()=>ws.send('O '+pc.localDescription.sdp)).catch(e=>console.log(e));
}
function signal(t){
  if(!pc) return;
  if(t.startsWith('A ')) pc.setRemoteDescription({type:'answer',sdp:t.slice(2)}).catch(e=>console.log(e));
  else if(t.startsWith('C ')){ const i=t.indexOf('\n'); pc.addIceCandidate({candidate:t.slice(2,i<0?undefined:i),sdpMid:i<0?'0':t.slice(i+1)}).catch(()=>{}); }
}
function send(m){
  if(dcOpen&&dc.bufferedAmount<65536) dc.send(m);
  else if(ws&&ws.readyState==1&&ws.bufferedAmount<65536) ws.send(m);
}

/* ---- incoming messages ---- */
let cur=null, fresh=false, lastId=-1, nframes=0, asm=new Map();
function magic(b){const u=new Uint8Array(b,0,4);return String.fromCharCode(u[0],u[1],u[2],u[3]);}
function binary(b){
  if(b.byteLength<12) return;
  const m=magic(b), dv=new DataView(b);
  if(m==='SUJ1'){
    const id=dv.getUint32(4,true), eye=dv.getUint8(8), eyes=dv.getUint8(9), ch=dv.getUint16(10,true), chs=dv.getUint16(12,true);
    if(id<=lastId) return;
    let a=asm.get(id);
    if(!a){a={eyes,parts:[[],[]],got:[0,0],need:[0,0],w:dv.getUint16(14,true),h:dv.getUint16(16,true),stereo:dv.getUint16(18,true)&1,png:dv.getUint16(18,true)&2,pose:new Float32Array(b.slice(20,48))}; asm.set(id,a);}
    if(a.parts[eye][ch]) return;
    a.parts[eye][ch]=new Uint8Array(b,48); a.got[eye]++; a.need[eye]=chs;
    for(let e=0;e<eyes;e++) if(!a.need[e]||a.got[e]<a.need[e]) return;
    for(const k of asm.keys()) if(k<=id) asm.delete(k);
    Promise.all(a.parts.slice(0,eyes).map(p=>createImageBitmap(new Blob(p,{type:a.png?'image/png':'image/jpeg'})))).then(bm=>{
      if(id<=lastId){bm.forEach(x=>x.close());return;}
      lastId=id; if(cur) cur.bm.forEach(x=>x.close());
      cur={bm,w:a.w,h:a.h,eyes,stereo:a.stereo,pose:a.pose}; fresh=true; nframes++;
    }).catch(()=>{});
    if(asm.size>8) for(const k of asm.keys()) if(k<id-8) asm.delete(k);
  } else if(m==='SUA1'){ if(audio) audio.play(0,dv.getUint16(8,true),new Int16Array(b.slice(12))); }
  else if(m==='SUM1'){ if(audio) audio.play(1,dv.getUint16(8,true),new Int16Array(b.slice(12))); }
}
)XR";
    s += R"XR(
/* ---- sound: game audio (stereo) and the other player's microphone (mono), each in an AudioWorklet ring buffer ---- */
const WORKLET=`
class Player extends AudioWorkletProcessor{
  constructor(o){super();this.ch=o.processorOptions.ch;this.len=this.ch*48000;this.buf=new Float32Array(this.len);
    this.r=0;this.n=0;this.frac=0;this.ratio=1;this.run=false;this.rate=48000;
    this.port.onmessage=e=>{const s=e.data.s;this.rate=e.data.rate;this.ratio=this.rate/sampleRate;
      let w=(this.r+this.n)%this.len;
      for(let i=0;i<s.length&&this.n<this.len;i++){this.buf[w]=s[i]/32768;w=(w+1)%this.len;this.n++;}
      if(!this.run&&this.n/this.ch>this.rate*0.06)this.run=true;
      if(this.n/this.ch>this.rate*0.25){const drop=(this.n/this.ch-Math.floor(this.rate*0.08))*this.ch;this.r=(this.r+drop)%this.len;this.n-=drop;}};}
  process(i,o){const L=o[0][0],R=o[0][1]||L,c=this.ch;
    for(let k=0;k<L.length;k++){
      if(!this.run||this.n<2*c){L[k]=0;R[k]=0;this.run=false;continue;}
      const a=this.r,b=(this.r+c)%this.len,f=this.frac;
      L[k]=this.buf[a]+(this.buf[b]-this.buf[a])*f;
      R[k]=c>1?this.buf[a+1]+(this.buf[(b+1)%this.len]-this.buf[a+1])*f:L[k];
      this.frac+=this.ratio;while(this.frac>=1){this.frac-=1;this.r=(this.r+c)%this.len;this.n-=c;}}
    return true;}}
registerProcessor('su-player',Player);
class Rec extends AudioWorkletProcessor{process(i){if(i[0]&&i[0][0])this.port.postMessage(i[0][0].slice(0));return true;}}
registerProcessor('su-rec',Rec);`;
let audio=null, audioStarting=false, micOn=false, micPeak=0;
async function startAudio(){
  if(audio||audioStarting||!$('snd').checked&&!$('mic').checked) return;
  audioStarting=true;
  const ctx=new AudioContext({latencyHint:'interactive'});
  await ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([WORKLET],{type:'application/javascript'})));
  const node=[2,1].map(ch=>{const n=new AudioWorkletNode(ctx,'su-player',{numberOfInputs:0,outputChannelCount:[2],processorOptions:{ch}});n.connect(ctx.destination);return n;});
  audio={ctx,play(k,rate,s){ if(k==0&&!$('snd').checked) return; node[k].port.postMessage({rate,s},[s.buffer]); }};
  if(ctx.state!=='running') ctx.resume();
  if($('mic').checked) startMic();
}
async function startMic(){
  if(micOn||!audio) return;
  micOn=true;
  let st;
  try{ st=await navigator.mediaDevices.getUserMedia({audio:{echoCancellation:true,noiseSuppression:true,autoGainControl:true}}); }
  catch(e){ micOn=false; $('st').textContent='microphone not available: '+e.message; return; }
  const ctx=audio.ctx, src=ctx.createMediaStreamSource(st), rec=new AudioWorkletNode(ctx,'su-rec');
  const mute=ctx.createGain(); mute.gain.value=0; src.connect(rec); rec.connect(mute); mute.connect(ctx.destination);
  const step=ctx.sampleRate/16000, out=new Int16Array(320); let pos=0, acc=0, cnt=0, n=0, seq=0;
  rec.port.onmessage=e=>{
    const x=e.data;
    for(let i=0;i<x.length;i++){
      acc+=x[i]; cnt++; pos+=1;
      if(pos>=step){ pos-=step; const v=Math.max(-1,Math.min(1,acc/cnt)); acc=0; cnt=0; out[n++]=v*32767; micPeak=Math.max(micPeak,Math.abs(v));
        if(n==320){ const b=new ArrayBuffer(12+640), d=new DataView(b); new Uint8Array(b).set([83,85,77,49]);
          d.setUint32(4,++seq,true); d.setUint16(8,16000,true); d.setUint16(10,320,true); new Int16Array(b,12).set(out); n=0;
          if($('mic').checked) send(b); } } }
  };
}
document.addEventListener('click',()=>{ startAudio(); });
$('mic').onchange=()=>{ if($('mic').checked) startAudio().then(startMic); };

connect();
setInterval(()=>{
  if(ws&&ws.readyState==1) $('st').textContent='connected over '+(dcOpen?'WebRTC (UDP)':'WebSocket (TCP)')+', '+nframes+' frames/s'
    +(cur?' ('+cur.w+'x'+cur.h+(cur.eyes==2?' stereo':' mono')+')':'')+(audio?', sound '+audio.ctx.state:', click for sound')
    +(micOn?', mic '+Math.round(micPeak*100)+'%':'');
  nframes=0; micPeak=0;
},1000);

/* desktop preview */
const pc2=$('prev'), p2=pc2.getContext('2d');
function preview(){
  if(fresh&&!session&&cur){
    fresh=false; const W=cur.w*cur.eyes;
    if(pc2.width!=W||pc2.height!=cur.h){pc2.width=W;pc2.height=cur.h;pc2.style.width=(W*1.5)+'px';}
    cur.bm.forEach((b,e)=>p2.drawImage(b,e*cur.w,0));
  }
  requestAnimationFrame(preview);
}
requestAnimationFrame(preview);
)XR";
    s += R"XR(
/* matrices (column-major) */
function mul(a,b){const r=new Float32Array(16);for(let c=0;c<4;c++)for(let i=0;i<4;i++){let s=0;for(let k=0;k<4;k++)s+=a[k*4+i]*b[c*4+k];r[c*4+i]=s;}return r;}
function T(x,y,z){return new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,x,y,z,1]);}
function S(x,y,z){return new Float32Array([x,0,0,0,0,y,0,0,0,0,z,0,0,0,0,1]);}
function RY(a){const c=Math.cos(a),s=Math.sin(a);return new Float32Array([c,0,-s,0,0,1,0,0,s,0,c,0,0,0,0,1]);}
function RX(a){const c=Math.cos(a),s=Math.sin(a);return new Float32Array([1,0,0,0,0,c,s,0,0,-s,c,0,0,0,0,1]);}
function angles(q){const x=q[0],y=q[1],z=q[2],w=q[3];
  const fx=-2*(x*z+w*y), fy=-2*(y*z-w*x), fz=-(1-2*(x*x+y*y));
  return [Math.atan2(-fx,-fz), Math.asin(Math.max(-1,Math.min(1,fy)))];}

/* WebXR */
let session=null, gl=null, ref=null, prog=null, texs=[], loc={}, imgW=0, imgH=0, lastBtn=0, poseFrame=null;
function checkXR(){ navigator.xr.isSessionSupported('immersive-vr').then(ok=>{$('vr').disabled=!ok; $('vr').textContent=ok?'Enter VR':(window.isSecureContext?'VR not available (start the headset runtime, e.g. SteamVR)':'VR needs https:// or localhost');}); }
if(navigator.xr){ checkXR(); navigator.xr.addEventListener('devicechange',checkXR); setInterval(()=>{ if(!session) checkXR(); },3000); }
else $('vr').textContent='WebXR not available (needs https or localhost)';
$('vr').onclick=async()=>{
  startAudio();
  session=await navigator.xr.requestSession('immersive-vr',{optionalFeatures:['local-floor']});
  const c=document.createElement('canvas');
  gl=c.getContext('webgl2',{xrCompatible:true,antialias:false});
  await gl.makeXRCompatible();
  session.updateRenderState({baseLayer:new XRWebGLLayer(session,gl),depthFar:1000});
  ref=await session.requestReferenceSpace('local');
  setupGL();
  session.onend=()=>{session=null; fresh=true;};
  session.requestAnimationFrame(onXR);
};
function setupGL(){
  const vs=`#version 300 es
in vec2 a; uniform mat4 P,V,M; out vec2 uv;
void main(){uv=vec2(a.x*0.5+0.5,0.5-a.y*0.5); gl_Position=P*V*M*vec4(a,0.0,1.0);}`;
  const fs=`#version 300 es
precision mediump float; in vec2 uv; uniform sampler2D t; out vec4 o;
void main(){o=texture(t,uv);}`;
  const sh=(t,src)=>{const s=gl.createShader(t);gl.shaderSource(s,src);gl.compileShader(s);return s;};
  prog=gl.createProgram(); gl.attachShader(prog,sh(gl.VERTEX_SHADER,vs)); gl.attachShader(prog,sh(gl.FRAGMENT_SHADER,fs)); gl.linkProgram(prog);
  for(const n of ['P','V','M','t']) loc[n]=gl.getUniformLocation(prog,n);
  const vb=gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER,vb);
  gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),gl.STATIC_DRAW);
  const al=gl.getAttribLocation(prog,'a'); gl.enableVertexAttribArray(al); gl.vertexAttribPointer(al,2,gl.FLOAT,false,0,0);
  texs=[0,1].map(()=>{const t=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,t);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);return t;});
}
function upload(){
  const f=cur; fresh=false; imgW=f.w; imgH=f.h; poseFrame=f.pose;
  for(let e=0;e<2;e++){
    gl.bindTexture(gl.TEXTURE_2D,texs[e]);
    gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,f.bm[f.eyes==2?e:0]);
  }
}
function sendInput(xf){
  const vp=xf.getViewerPose(ref); if(!vp) return null;
  const t=vp.transform, h=[t.position.x,t.position.y,t.position.z,t.orientation.x,t.orientation.y,t.orientation.z,t.orientation.w];
  let src=null;
  for(const s of session.inputSources) if(s.gamepad&&(!src||s.handedness==='right')) src=s;
  let hand=[0,0,0,0,0,0,1], have=0, bits=0, recentre=false;
  if(src){
    const p=xf.getPose(src.targetRaySpace,ref);
    if(p){const q=p.transform; hand=[q.position.x,q.position.y,q.position.z,q.orientation.x,q.orientation.y,q.orientation.z,q.orientation.w]; have=1;}
    const b=src.gamepad.buttons, pr=i=>b[i]&&b[i].pressed;
    bits=(pr(0)?1:0)|(pr(1)?2:0)|(pr(4)?4:0)|(pr(5)?8:0);
    const ax=src.gamepad.axes; if(ax&&ax.length>3&&Math.abs(ax[3])>0.5) fov=Math.max(20,Math.min(140,fov+ax[3]*0.5));
  }
  for(const s of session.inputSources) if(s.gamepad&&s!==src&&s.gamepad.buttons[5]&&s.gamepad.buttons[5].pressed) bits|=8;
  if((bits&8)&&!(lastBtn&8)) recentre=true;
  lastBtn=bits;
  if(recentre&&ws&&ws.readyState==1) ws.send('R');
  send('P '+h.map(v=>v.toFixed(5)).join(' ')+' '+have+' '+hand.map(v=>v.toFixed(5)).join(' ')+' '+(bits&7));
  return vp;
}
function onXR(time,xf){
  session.requestAnimationFrame(onXR);
  const vp=sendInput(xf); if(!vp) return;
  if(fresh&&cur) upload();
  const layer=session.renderState.baseLayer;
  gl.bindFramebuffer(gl.FRAMEBUFFER,layer.framebuffer);
  gl.clearColor(0,0,0,1); gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
  if(!imgW) return;
  const ang=angles([poseFrame[3],poseFrame[4],poseFrame[5],poseFrame[6]]);
  const asp=aspectQ||imgW/imgH, hw=DIST*Math.tan(fov*Math.PI/360), hh=hw/asp;
  const M=mul(mul(mul(mul(T(poseFrame[0],poseFrame[1],poseFrame[2]),RY(ang[0])),RX(ang[1])),T(0,0,-DIST)),S(hw,hh,1));
  gl.useProgram(prog); gl.uniformMatrix4fv(loc.M,false,M); gl.uniform1i(loc.t,0); gl.activeTexture(gl.TEXTURE0);
  for(const v of vp.views){
    const r=layer.getViewport(v); gl.viewport(r.x,r.y,r.width,r.height);
    gl.uniformMatrix4fv(loc.P,false,v.projectionMatrix); gl.uniformMatrix4fv(loc.V,false,v.transform.inverse.matrix);
    gl.bindTexture(gl.TEXTURE_2D,texs[v.eye==='right'?1:0]);
    gl.drawArrays(gl.TRIANGLE_STRIP,0,4);
  }
}
</script></body></html>
)XR";
    return s;
}

#endif
