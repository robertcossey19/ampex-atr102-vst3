const C = require('./dsp-core.js');
const fs = +process.argv[2] || 48000;
function run(cfg, f0, dbVU, secs, silence=false, hiss=false) {
  const ch = new C.TapeChannel(fs, 512); ch.configure(cfg);
  const block=512, total=Math.floor(fs*secs); const out=new Float32Array(total); let ph=0; const a=Math.pow(10,dbVU/20);
  const x=new Float32Array(block), nz=new Float32Array(block); let rng=12345;
  const gauss=()=>{let s=0;for(let k=0;k<4;k++){rng^=rng<<13;rng^=rng>>>17;rng^=rng<<5;rng>>>=0;s+=((rng&0xFFFFFF)/8388608)-1;}return s*0.8660254;};
  for(let done=0;done<total;done+=block){ const n=Math.min(block,total-done);
    for(let i=0;i<n;i++){ x[i]=silence?0:a*Math.sin(ph); ph+=2*Math.PI*f0/fs; if(hiss) nz[i]=gauss(); }
    ch.process(x,n,hiss,nz); out.set(x.subarray(0,n),done); }
  return out;
}
function mag(y,f,skip){ const N=y.length-skip; let re=0,im=0; for(let n=0;n<N;n++){const w=0.5-0.5*Math.cos(2*Math.PI*n/(N-1)); const a=-2*Math.PI*f*n/fs; re+=y[n+skip]*w*Math.cos(a); im+=y[n+skip]*w*Math.sin(a);} return Math.hypot(re,im)*4/N; }
function thd(y,f0,skip){ const f=mag(y,f0,skip); let h=0; for(let k=2;k<=10;k++) if(f0*k<fs/2){const m=mag(y,f0*k,skip); h+=m*m;} return 100*Math.sqrt(h)/f; }
const dB=x=>20*Math.log10(x);
const skip=Math.floor(fs*0.25);
const base={tape:1,speed:1,eq:0,flux:2,biasDb:0,transformer:true,vibe:false,mono:false};
console.log('== response (dB re in), 456/370, small signal ==');
for (const [sp,eq] of [[0,0],[0,1],[1,0],[1,1],[2,0]]) { let s=`${C.SPEEDS[sp]} ${C.eqFor(sp,eq).name}:`; for (const f of [20,30,50,100,1000,5000,10000,15000,20000]) { if(f>=fs*0.45) continue; const y=run({...base,speed:sp,eq},f,-30,0.5); s+=` ${f}:${dB(mag(y,f,skip)/Math.pow(10,-30/20)).toFixed(2)}`; } console.log(s); }
console.log('== 1 kHz THD vs level ==');
for (const vu of [-10,0,6,9,12,15]) { const y=run(base,1000,vu,0.6); console.log(`  ${vu} VU: out ${dB(mag(y,1000,skip)/Math.pow(10,vu/20)).toFixed(2)} dB  THD ${thd(y,1000,skip).toFixed(3)}%`); }
console.log('== stocks: THD@0VU, 3% point ==');
for (let t=0;t<6;t++){ const y0=run({...base,tape:t},1000,0,0.6); let lo=0,hi=18,mid=9; for(let it=0;it<10;it++){mid=0.5*(lo+hi); const y=run({...base,tape:t},1000,mid,0.5); if(thd(y,1000,skip)>3) hi=mid; else lo=mid;} console.log(`  ${C.TAPES[t].name} THD@0VU ${thd(y0,1000,skip).toFixed(3)}%  3% at +${mid.toFixed(1)} VU`); }
console.log('== HF sat: 10k vs 1k at +9 VU ==');
for (let sp=0;sp<3;sp++){ const a=run({...base,speed:sp},1000,9,0.5), b=run({...base,speed:sp},10000,9,0.5); console.log(`  ${C.SPEEDS[sp]} ips: 1k ${dB(mag(a,1000,skip)/Math.pow(10,9/20)).toFixed(2)}  10k ${dB(mag(b,10000,skip)/Math.pow(10,9/20)).toFixed(2)}`); }
console.log('== bias ==');
for (const b of [-5,-3,0,3,5]) { const a=run({...base,biasDb:b},1000,0,0.5), c=run({...base,biasDb:b},10000,0,0.5); console.log(`  bias ${b}: 1k ${dB(mag(a,1000,skip)).toFixed(2)} 10k ${dB(mag(c,10000,skip)).toFixed(2)} THD ${thd(a,1000,skip).toFixed(3)}%`); }
console.log('== hiss (dB re 0 VU rms) ==');
for (let sp=0;sp<3;sp++){ const y=run({...base,speed:sp},1000,-200,1.0,true,true); let s=0; for(let i=skip;i<y.length;i++) s+=y[i]*y[i]; console.log(`  ${C.SPEEDS[sp]} ips: ${(dB(Math.sqrt(s/(y.length-skip)))-dB(0.7071)).toFixed(1)}`); }
console.log('== OS filter: 20 kHz tone at -30 VU through chain, alias check at 48k-20k... (level) ==');
{ const y=run(base,19000,-30,0.5); console.log('  19k level', dB(mag(y,19000,skip)/Math.pow(10,-30/20)).toFixed(2), ' alias @10k', dB(mag(y,10000,skip)).toFixed(1)); }
{ const t0=Date.now(); run(base,1000,0,5); console.log('CPU: 5 s mono in', (Date.now()-t0)/1000, 's'); }
