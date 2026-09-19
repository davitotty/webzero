"""Native socket regression suite, dependency-free. Also used by bench/native.py."""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

RAW = b'hello world ' * 100
BR = bytes.fromhex('1baf04f88d946ede44558696206c6f358b63b5400654db0b00')
LARGE = bytes(range(256)) * (4 * 1024 * 1024 // 256)

def fixture(version=2, wide=True):
    assets = [('index', RAW, BR, 'text/html', 1), ('large.bin', LARGE, LARGE, 'application/octet-stream', 0),
              ('hero.png', b'PNG', b'PNG', 'image/png', 0), ('hero.webp', b'WEBP', b'WEBP', 'image/webp', 0)]
    if wide:
        assets += [('page%d' % i, ('page%d' % i).encode(), ('page%d' % i).encode(), 'text/html', 0) for i in range(20)]
    count = len(assets)
    nodes = count + 3  # root, assets, docs, docs/index
    trie = bytearray(nodes * 64)
    for i in range(nodes):
        struct.pack_into('<ii', trie, i*64+(40 if version==2 else 50), -1, -1)
        if version==2: struct.pack_into('<II',trie,i*64+32,0xffffffff,0xffffffff)
    for i,(name,*_) in enumerate(assets,1):
        trie[i*64:i*64+len(name)] = name.encode()
        struct.pack_into('<i',trie,i*64+(40 if version==2 else 50),i-1)
    docs = count+1
    trie[docs*64:docs*64+4] = b'docs'
    trie[(docs+1)*64:(docs+1)*64+5] = b'index'
    struct.pack_into('<i',trie,(docs+1)*64+(40 if version==2 else 50),0)
    if version==2:
        struct.pack_into('<I',trie,32,1)
        for i in range(1,docs): struct.pack_into('<I',trie,i*64+36,i+1)
        struct.pack_into('<I',trie,docs*64+32,docs+1)
    else:
        children = list(range(1,docs+1))[:8]
        struct.pack_into('<H',trie,32,len(children))
        for j,i in enumerate(children): struct.pack_into('<H',trie,34+j*2,i)
        struct.pack_into('<HH',trie,docs*64+32,1,docs+1)
    table = bytearray(count*56)
    payload = bytearray()
    for i,(_,raw,data,mime,enc) in enumerate(assets):
        struct.pack_into('<III',table,i*56,len(payload),len(data),len(raw))
        table[i*56+12:i*56+12+len(mime)] = mime.encode()
        table[i*56+44] = enc
        struct.pack_into('<i',table,i*56+48,3 if i==2 else -1)
        payload.extend(data)
        if version==2 and enc:
            struct.pack_into('<I',table,i*56+52,len(payload)); payload.extend(raw)
    config = bytearray(96); config[:9] = b'localhost'
    struct.pack_into('<HHIIII',config,64,8080,256,1500,count,0,nodes)
    asset_off = 28+len(trie); end = asset_off+len(table)+len(payload)
    header = struct.pack('<7I',0x57454230,version,28,asset_off,end,end,end+96)
    return bytearray(header+trie+table+payload+config)

class Server:
    def __init__(self, binary, bundle):
        with socket.socket() as s: s.bind(('127.0.0.1',0)); self.port = s.getsockname()[1]
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([str(Path(binary).resolve()),str(bundle),str(self.port)],stdout=self.log,stderr=self.log)
        for _ in range(200):
            if self.process.poll() is not None: raise RuntimeError(self.output())
            try:
                with socket.create_connection(('127.0.0.1',self.port),timeout=.1): return
            except OSError: time.sleep(.02)
        self.close(); raise RuntimeError('server did not start')
    def output(self):
        self.log.seek(0); return self.log.read().decode(errors='replace')
    def close(self):
        self.process.terminate()
        try: self.process.wait(timeout=5)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        output=self.output(); self.log.close()
        if 'ERROR: AddressSanitizer' in output or 'runtime error:' in output: raise AssertionError(output)
    def request(self,path='/',method='GET',headers=None):
        c=http.client.HTTPConnection('127.0.0.1',self.port,timeout=5)
        try:
            c.request(method,path,headers=headers or {}); r=c.getresponse()
            return r.status,dict(r.getheaders()),r.read()
        finally: c.close()
    def socket(self): return socket.create_connection(('127.0.0.1',self.port),timeout=5)

def run(binary, baseline=False):
    results=[]
    def check(name, fn):
        try:
            assert fn(), name
            results.append({'name':name,'passed':True})
        except Exception as e: results.append({'name':name,'passed':False,'error':str(e)})
    with tempfile.TemporaryDirectory(prefix='webzero-native-') as tmp:
        file=Path(tmp)/'test.web'; file.write_bytes(fixture(1 if baseline else 2))
        server=Server(binary,file)
        def response(path='/',method='GET',headers=None): return server.request(path,method,headers)
        def raw_status(data):
            with server.socket() as s:
                s.sendall(data); return s.recv(4096).split(b' ')[1]
        try:
            check('identity bytes',lambda:response()[2]==RAW)
            check('Brotli bytes',lambda:response(headers={'Accept-Encoding':'br'})[2]==BR)
            check('Brotli q=0',lambda:response(headers={'Accept-Encoding':'br;q=0,*;q=1'})[2]==RAW)
            check('encoding exact token',lambda:response(headers={'Accept-Encoding':'zebra'})[2]==RAW)
            check('406 when all encodings excluded',lambda:response(headers={'Accept-Encoding':'identity;q=0,br;q=0'})[0]==406)
            check('HEAD metadata',lambda:response(method='HEAD')[1].get('Content-Type')=='text/html' and response(method='HEAD')[2]==b'')
            check('HEAD missing route no body',lambda:response('/missing','HEAD')[0]==404 and response('/missing','HEAD')[2]==b'')
            check('POST static rejected',lambda:response(method='POST')[0]==405)
            check('method prefix rejected',lambda:raw_status(b'GETTING / HTTP/1.1\r\nHost: t\r\n\r\n')==b'405')
            check('HTML alias',lambda:response('/index.html')[2]==RAW)
            check('directory index',lambda:response('/docs/')[2]==RAW)
            check('20 sibling routes',lambda:all(response('/page%d'%i)[2]==('page%d'%i).encode() for i in range(20)))
            check('WebP negotiated',lambda:response('/hero.png',headers={'Accept':'image/webp'})[2]==b'WEBP')
            check('WebP q=0',lambda:response('/hero.png',headers={'Accept':'image/webp;q=0'})[2]==b'PNG')
            tag=response()[1].get('ETag','')
            check('ETag 304',lambda:response(headers={'If-None-Match':tag})[0]==304)
            check('ETag list',lambda:response(headers={'If-None-Match':'"other", '+tag})[0]==304)
            check('byte range',lambda:response(headers={'Range':'bytes=6-10'})[2]==RAW[6:11] and response(headers={'Range':'bytes=6-10'})[0]==206)
            check('suffix range',lambda:response(headers={'Range':'bytes=-10'})[2]==RAW[-10:])
            check('unsatisfiable range',lambda:response(headers={'Range':'bytes=999999-'})[0]==416)
            check('If-Range falls back to full body',lambda:response(headers={'Range':'bytes=1-2','If-Range':'"x"'})[2]==RAW)
            check('invalid percent escape',lambda:response('/%zz')[0]==400)
            check('encoded traversal rejected',lambda:response('/%2e%2e/x')[0]==400)
            check('encoded NUL rejected',lambda:response('/%00')[0]==400)
            check('oversized URI',lambda:response('/'+'a'*512)[0]==414)
            check('missing Host',lambda:raw_status(b'GET / HTTP/1.1\r\n\r\n')==b'400')
            check('duplicate Host',lambda:raw_status(b'GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n')==b'400')
            check('duplicate Content-Length',lambda:raw_status(b'GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n')==b'400')
            check('ambiguous transfer framing',lambda:raw_status(b'GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n')==b'400')
            check('chunked rejected explicitly',lambda:raw_status(b'GET / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n')==b'501')
            check('oversized header',lambda:raw_status(b'GET / HTTP/1.1\r\nHost: a\r\nX-Test: '+b'a'*8200+b'\r\n\r\n')==b'431')
            check('large transfer exact bytes',lambda:response('/large.bin')[2]==LARGE)
            def pipeline():
                with server.socket() as s:
                    s.sendall(b'GET / HTTP/1.1\r\nHost: t\r\n\r\n'*3+b'GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n')
                    data=b''
                    while True:
                        chunk=s.recv(65536)
                        if not chunk: break
                        data+=chunk
                    return data.count(b'HTTP/1.1 200')==4 and data.count(RAW)==4
            check('four pipelined requests',pipeline)
            def fragmented():
                with server.socket() as s:
                    s.sendall(b'GET / HTTP/1.1\r\nHo');time.sleep(.02);s.sendall(b'st: t\r\nConnection: close\r\n\r\n')
                    data=b''
                    while True:
                        chunk=s.recv(65536)
                        if not chunk: break
                        data+=chunk
                    return data.endswith(RAW)
            check('fragmented headers',fragmented)
            def post_pipeline():
                with server.socket() as s:
                    s.sendall(b'POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\na');time.sleep(.02)
                    s.sendall(b'bcGET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n')
                    data=b''
                    while True:
                        chunk=s.recv(65536)
                        if not chunk:break
                        data+=chunk
                    return data.count(b'HTTP/1.1 ')==2 and b'405' in data and data.endswith(RAW)
            check('body framing preserves next request',post_pipeline)
            def slow_reader():
                with server.socket() as s:
                    s.sendall(b'GET /large.bin HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n');time.sleep(.1)
                    assert response()[2]==RAW
                    data=bytearray()
                    while True:
                        chunk=s.recv(32768)
                        if not chunk:break
                        data.extend(chunk)
                    return bytes(data).split(b'\r\n\r\n',1)[1]==LARGE
            check('slow reader backpressure and concurrent request',slow_reader)
            def parallel():
                from concurrent.futures import ThreadPoolExecutor
                with ThreadPoolExecutor(max_workers=32) as pool:return all(pool.map(lambda _:response()[2]==RAW,range(96)))
            check('96 requests over 32 clients',parallel)
            def idle():
                with server.socket() as s:
                    s.sendall(b'GET /');time.sleep(1.8);return s.recv(1)==b''
            check('idle incomplete connection expires',idle)
        finally:server.close()
        legacy=Path(tmp)/'legacy.web';legacy.write_bytes(fixture(1,False)); old=Server(binary,legacy)
        try:
            check('v1 Brotli compatibility',lambda:old.request(headers={'Accept-Encoding':'br'})[2]==BR)
            check('v1 missing identity returns 406',lambda:old.request()[0]==406)
        finally:old.close()
        original=fixture()
        mutations=[('bad magic',0,0),('bad config offset',20,0xfffffff0),('bad asset offset',struct.unpack_from('<I',original,12)[0],0xffffffff),
                   ('route self cycle',28+32,0),('invalid asset index',28+64+40,9999),('oversized count',len(original)-96+80,9999)]
        for name,off,value in mutations:
            bad=bytearray(original);struct.pack_into('<I',bad,off,value);file.write_bytes(bad)
            def rejected():
                p=subprocess.run([str(Path(binary).resolve()),str(file)],capture_output=True,timeout=5)
                return p.returncode==1 and b'Sanitizer' not in p.stderr and b'runtime error:' not in p.stderr
            check('reject '+name,rejected)
    print(json.dumps({'passed':sum(r['passed'] for r in results),'total':len(results),'results':results},indent=2))
    return all(r['passed'] for r in results)
if __name__=='__main__':sys.exit(0 if run(sys.argv[1] if len(sys.argv)>1 else './webzero', '--baseline' in sys.argv) else 1)
