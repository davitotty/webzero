"""Exercise native VM responses through the real request/connection pipeline."""
from native import Server
from pathlib import Path
import http.client
import struct
import sys
import tempfile

def bundle(code):
    trie=bytearray(128)
    struct.pack_into('<IIii',trie,32,1,0xffffffff,-1,-1)
    trie[64:69]=b'index'
    struct.pack_into('<IIii',trie,96,0xffffffff,0xffffffff,-1,0)
    handler=struct.pack('<II',28+128+8,len(code))
    config=bytearray(96);config[:9]=b'localhost';struct.pack_into('<HHIIII',config,64,8080,256,1000,0,1,2)
    end=28+128+8+len(code)
    return struct.pack('<7I',0x57454230,2,28,156,156,end,end+96)+trie+handler+code+config

def run(binary):
    cases=[('body reaches VM',bytes([3,3,4,1,0]),'POST',200,b'hello'),
           ('HEAD has no dynamic body',bytes([1,3,0])+b'hey'+bytes([4,1,0]),'HEAD',200,b''),
           ('infinite loop is bounded',bytes([7,253,255]),'GET',500,b''),
           ('redirect injection rejected',bytes([1,6,0])+b'/x\r\nX:'+bytes([10]),'GET',500,b''),
           ('stack underflow rejected',bytes([5]),'GET',500,b'')]
    with tempfile.TemporaryDirectory() as tmp:
        path=Path(tmp)/'handler.web'
        for name,code,method,status,body in cases:
            path.write_bytes(bundle(code));server=Server(binary,path)
            try:
                client=http.client.HTTPConnection('127.0.0.1',server.port,timeout=3)
                client.request(method,'/',body=b'hello' if method=='POST' else None)
                response=client.getresponse();assert response.status==status and response.read()==body,name
                client.close();print('PASS:',name)
            finally:server.close()
if __name__=='__main__':run(sys.argv[1])
