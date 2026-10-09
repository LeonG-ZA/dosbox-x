/*
 *  WebXR page served by xrserver.cpp. Protocol: see xrserver.cpp.
 *
 *  Each eye's PIX image is drawn on a large quad placed with the head pose the frame was rendered for (yaw and pitch
 *  only), so head motion between emulator frames is corrected by the headset (rotational time-warp) and head roll
 *  keeps the horizon level. URL parameters: player=1|2, fov=<horizontal degrees>, aspect=<width/height, default drawn width / height>.
 *  (MSVC limits one string literal to 16 KB, hence the pieces.)
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
#prev{image-rendering:pixelated;max-width:100%;background:#000}
#st{color:#8c8}
</style></head><body>
<h1>SU2000 VR</h1>
<div id="bar">
<label>Player <select id="pl"><option>1</option><option>2</option></select></label>
<label>FOV <input id="fov" type="number" min="20" max="140" value="60" style="width:4em">&deg;</label>
<button id="vr" disabled>Enter VR</button>
<span id="st">connecting...</span>
</div>
<canvas id="prev" width="384" height="250"></canvas>
<p>Trigger = fire, grip or A/X = walk, B/Y = recentre, thumbstick up/down = field of view.</p>
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
let ws=null, frame=null, fresh=false, nframes=0, cropW=8, cropFor=0;  /* the game may draw into only part of the line: crop to the widest drawn column seen */
function connect(){
  ws=new WebSocket('ws://'+location.host+'/ws'); ws.binaryType='arraybuffer';
  ws.onopen=()=>{ws.send('S '+player); $('st').textContent='connected';};
  ws.onclose=()=>{$('st').textContent='disconnected, retrying'; setTimeout(connect,1000);};
  ws.onmessage=e=>{
    if(typeof e.data==='string') return;
    const b=e.data, u=new Uint16Array(b,4,4);
    frame={w:u[0],h:u[1],eyes:u[2],stereo:u[3]&1,pose:new Float32Array(b,12,7),px:new Uint16Array(b,40)};
    const f=frame; if(f.w!=cropFor){cropFor=f.w; cropW=8;}
    for(let y=0;y<f.h;y+=4){const r=y*f.w; for(let x=f.w-1;x>=cropW;x--) if(f.px[r+x]){cropW=Math.min(f.w,(x+8)&~7); break;}}
    fresh=true; nframes++;
  };
}
connect();
setInterval(()=>{ if(ws&&ws.readyState==1) $('st').textContent='connected, '+nframes+' frames/s'+(frame?' ('+frame.w+'x'+frame.h+(frame.eyes==2?' stereo':' mono')+', drawn width '+cropW+')':''); nframes=0; },1000);

/* desktop preview */
const pc=$('prev'), p2=pc.getContext('2d'); let img=null;
function preview(){
  if(fresh && !session){
    fresh=false; const f=frame, W=f.w*f.eyes;
    if(pc.width!=W||pc.height!=f.h){pc.width=W;pc.height=f.h;pc.style.width=W+'px';img=null;}
    if(!img) img=p2.createImageData(W,f.h);
    const d=img.data;
    for(let e=0;e<f.eyes;e++) for(let y=0;y<f.h;y++) for(let x=0;x<f.w;x++){
      const v=f.px[(e*f.h+y)*f.w+x], o=4*(y*W+e*f.w+x);
      d[o]=(v>>8)&0xF8; d[o+1]=(v>>3)&0xFC; d[o+2]=(v<<3)&0xF8; d[o+3]=255;
    }
    p2.putImageData(img,0,0);
  }
  requestAnimationFrame(preview);
}
requestAnimationFrame(preview);

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
function checkXR(){ navigator.xr.isSessionSupported('immersive-vr').then(ok=>{$('vr').disabled=!ok; $('vr').textContent=ok?'Enter VR':'VR not available (start the headset runtime, e.g. SteamVR)';}); }
if(navigator.xr){ checkXR(); navigator.xr.addEventListener('devicechange',checkXR); setInterval(()=>{ if(!session) checkXR(); },3000); }
else $('vr').textContent='WebXR not available (needs https or localhost)';
$('vr').onclick=async()=>{
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
in vec2 a; uniform mat4 P,V,M; uniform float U; out vec2 uv;
void main(){uv=vec2((a.x*0.5+0.5)*U,0.5-a.y*0.5); gl_Position=P*V*M*vec4(a,0.0,1.0);}`;
  const fs=`#version 300 es
precision mediump float; in vec2 uv; uniform sampler2D t; out vec4 o;
void main(){o=texture(t,uv);}`;
  const sh=(t,src)=>{const s=gl.createShader(t);gl.shaderSource(s,src);gl.compileShader(s);return s;};
  prog=gl.createProgram(); gl.attachShader(prog,sh(gl.VERTEX_SHADER,vs)); gl.attachShader(prog,sh(gl.FRAGMENT_SHADER,fs)); gl.linkProgram(prog);
  for(const n of ['P','V','M','t','U']) loc[n]=gl.getUniformLocation(prog,n);
  const vb=gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER,vb);
  gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),gl.STATIC_DRAW);
  const al=gl.getAttribLocation(prog,'a'); gl.enableVertexAttribArray(al); gl.vertexAttribPointer(al,2,gl.FLOAT,false,0,0);
  texs=[0,1].map(()=>{const t=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,t);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);return t;});
  gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);
}
function upload(){
  const f=frame; fresh=false; imgW=f.w; imgH=f.h; poseFrame=f.pose;
  for(let e=0;e<2;e++){
    const src=f.px.subarray((f.eyes==2?e:0)*f.w*f.h,((f.eyes==2?e:0)+1)*f.w*f.h);
    gl.bindTexture(gl.TEXTURE_2D,texs[e]);
    gl.texImage2D(gl.TEXTURE_2D,0,gl.RGB565,f.w,f.h,0,gl.RGB,gl.UNSIGNED_SHORT_5_6_5,src);
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
  if(ws&&ws.readyState==1&&ws.bufferedAmount<4096){
    if(recentre) ws.send('R');
    ws.send('P '+h.map(v=>v.toFixed(5)).join(' ')+' '+have+' '+hand.map(v=>v.toFixed(5)).join(' ')+' '+(bits&7));
  }
  return vp;
}
function onXR(time,xf){
  session.requestAnimationFrame(onXR);
  const vp=sendInput(xf); if(!vp) return;
  if(fresh&&frame) upload();
  const layer=session.renderState.baseLayer;
  gl.bindFramebuffer(gl.FRAMEBUFFER,layer.framebuffer);
  gl.clearColor(0,0,0,1); gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
  if(!imgW) return;
  const ang=angles([poseFrame[3],poseFrame[4],poseFrame[5],poseFrame[6]]);
  const asp=aspectQ||cropW/imgH, hw=DIST*Math.tan(fov*Math.PI/360), hh=hw/asp;
  const M=mul(mul(mul(mul(T(poseFrame[0],poseFrame[1],poseFrame[2]),RY(ang[0])),RX(ang[1])),T(0,0,-DIST)),S(hw,hh,1));
  gl.useProgram(prog); gl.uniformMatrix4fv(loc.M,false,M); gl.uniform1i(loc.t,0); gl.uniform1f(loc.U,cropW/imgW); gl.activeTexture(gl.TEXTURE0);
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
