'use strict';
/* Reproducible, dependency-free benchmark. No network outside loopback. */
const fs=require('fs'),path=require('path'),os=require('os'),http=require('http'),net=require('net'),crypto=require('crypto');
const {spawn,spawnSync}=require('child_process');
const {performance}=require('perf_hooks');
const root=path.resolve(__dirname,'..'), baseline=path.resolve(process.argv[2] || path.join(root,'..','webzero-baseline'));
const out=path.join(__dirname,'results');fs.mkdirSync(out,{recursive:true});
const median=a=>[...a].sort((a,b)=>a-b)[Math.floor(a.length/2)];
function build(cli,dir,dest,quality) {
    const args=[cli,'build',dir];if(quality!==undefined)args.push('--quality',String(quality));
    const start=performance.now(),r=spawnSync(process.execPath,args,{encoding:'utf8',timeout:120000,windowsHide:true});
    const ms=performance.now()-start;
    if(r.status!==0)throw new Error(r.stderr || r.stdout);
    fs.copyFileSync(dir+'.web',dest);
    return {ms,bytes:fs.statSync(dest).size};
}
async function freePort(){return new Promise(resolve=>{const s=net.createServer();s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>resolve(p));});});}
async function start(cli,bundle){
    const port=await freePort(),child=spawn(process.execPath,[cli,'serve',bundle,'--port',String(port),'--js'],{stdio:['ignore','pipe','pipe'],windowsHide:true});
    let output='';child.stdout.on('data',b=>output+=b);child.stderr.on('data',b=>output+=b);
    for(let i=0;i<200;i++){
        if(child.exitCode!==null)throw new Error(output);
        if(await new Promise(resolve=>{const s=net.connect(port,'127.0.0.1');s.once('connect',()=>{s.destroy();resolve(true);});s.once('error',()=>resolve(false));}))return {child,port};
        await new Promise(r=>setTimeout(r,25));
    }
    child.kill();throw new Error('Server startup timeout: '+output);
}
async function stop(child){if(child.exitCode!==null)return;await new Promise(resolve=>{child.once('exit',resolve);child.kill();});}
async function load(port,expected,count=5000,concurrency=16,encoding='identity'){
    const agent=new http.Agent({keepAlive:true,maxSockets:concurrency}),samples=[];
    let next=0,errors=0,bytes=0;
    const start=performance.now();
    async function worker(){
        while(next++<count){
            const t=performance.now();
            await new Promise(resolve=>{
                let finished=false;
                const finish=ok=>{if(finished)return;finished=true;if(!ok)errors++;samples.push(performance.now()-t);resolve();};
                const req=http.get({host:'127.0.0.1',port,path:'/',agent,headers:{'Accept-Encoding':encoding}},res=>{
                    const chunks=[];res.on('data',b=>chunks.push(b));res.on('error',()=>finish(false));res.on('end',()=>{
                        const body=Buffer.concat(chunks);bytes+=body.length;finish(res.statusCode===200&&body.equals(expected));
                    });
                });req.setTimeout(5000,()=>req.destroy());req.on('error',()=>finish(false));
            });
        }
    }
    await Promise.all(Array.from({length:concurrency},worker));
    const elapsed=performance.now()-start;agent.destroy();samples.sort((a,b)=>a-b);
    return {requests:count,concurrency,elapsedMs:elapsed,successfulRps:(count-errors)*1000/elapsed,
        p50Ms:samples[Math.floor(samples.length*.5)],p95Ms:samples[Math.floor(samples.length*.95)],errors,bytes};
}
async function main(){
    const original=path.join(baseline,'tools','wz.js'),current=path.join(root,'tools','wz.js');
    if(!fs.existsSync(original))throw new Error('Pass a checkout of baseline 1d0d262 as the first argument');
    const site=path.join(out,'landing');fs.mkdirSync(site,{recursive:true});
    for(const file of ['index.html','1920-1080-sample.png'])fs.copyFileSync(path.join(baseline,'examples','landing-page',file),path.join(site,file));
    const report={date:new Date().toISOString(),baselineCommit:'1d0d262',environment:{node:process.version,platform:process.platform,arch:process.arch,cpu:os.cpus()[0].model},
        methodology:'Three alternating builds of the unchanged example landing page. Default old quality 11 versus new 5, plus new quality 11. HTTP: same Node executable, 16 keep-alive clients, 5000 requests per trial, 5 alternating trials, warmup 500; every body compared byte-for-byte.',builds:{old:[],new:[],newQuality11:[]},http:{old:[],new:[]}};
    for(let i=0;i<3;i++){
        const order=i%2?['new','old']:['old','new'];
        for(const name of order)report.builds[name].push(build(name==='old'?original:current,site,path.join(out,name+'-landing.web')));
        report.builds.newQuality11.push(build(current,site,path.join(out,'new-q11-landing.web'),11));
    }
    const perf=path.join(out,'payload');fs.mkdirSync(perf,{recursive:true});
    const body=Buffer.from('<!doctype html><title>WebZero benchmark</title>\n'+Array.from({length:800},(_,i)=>`<p id="section-${i}">WebZero serves prebuilt content on small machines. Entry ${i}, deterministic payload.</p>\n`).join(''));
    fs.writeFileSync(path.join(perf,'index.html'),body);
    build(original,perf,path.join(out,'old-payload.web'));build(current,perf,path.join(out,'new-payload.web'));
    report.http.payloadBytes=body.length;report.http.payloadSha256=crypto.createHash('sha256').update(body).digest('hex');
    for(let i=0;i<5;i++)for(const name of (i%2?['new','old']:['old','new'])){
        const {child,port}=await start(name==='old'?original:current,path.join(out,name+'-payload.web'));
        try{await load(port,body,500);report.http[name].push(await load(port,body));}finally{await stop(child);}
    }
    report.summary={};
    for(const name of ['old','new','newQuality11'])report.summary[name+'BuildMs']=median(report.builds[name].map(x=>x.ms));
    for(const name of ['old','new']){
        report.summary[name+'BundleBytes']=report.builds[name][0].bytes;
        report.summary[name+'Rps']=median(report.http[name].map(x=>x.successfulRps));
        report.summary[name+'P95Ms']=median(report.http[name].map(x=>x.p95Ms));
        report.summary[name+'Errors']=report.http[name].reduce((n,x)=>n+x.errors,0);
    }
    fs.writeFileSync(path.join(out,'comparison.json'),JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report.summary,null,2));
}
main().catch(e=>{console.error(e);process.exitCode=1;});
