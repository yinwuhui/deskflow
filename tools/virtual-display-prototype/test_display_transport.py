# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import socket
import ssl
import struct
import tempfile
import threading
import time
import unittest

from display_transport import (BIND, HELLO, PING, VIDEO, BindingTokens, Frame, Reader,
                               TransportError, authenticate, decode_json, fingerprint,
                               init_identity, require_fields, tls_context, TransferMetrics, tcp_rtt_ms)
from h264_access_units import AccessUnitSplitter, MAX_AU, nal_types
from display_session import BoundedUnits


class MetricTests(unittest.TestCase):
    def test_passive_kernel_rtt_units_and_incomplete_results(self):
        class Wire:
            def __init__(self, data): self.data=data
            def getsockopt(self, *args): return self.data
        linux=struct.pack('=8B16I',1,*([0]*7),*([0]*15),12500)
        mac=struct.pack('=4B11I',4,0,0,0,*([0]*10),12)
        self.assertEqual(tcp_rtt_ms(Wire(linux),'linux'),12.5)
        self.assertEqual(tcp_rtt_ms(Wire(mac),'darwin'),12)
        self.assertIsNone(tcp_rtt_ms(Wire(b'bad'),'linux'))
        self.assertIsNone(tcp_rtt_ms(Wire(linux),'unsupported'))

    def test_rates_use_elapsed_time_and_real_local_byte_counts(self):
        class Wire:
            def getsockopt(self,*args): raise OSError('unavailable')
        class Channel:
            tx_bytes=0; rx_bytes=0; wire=Wire()
        c=Channel();clock=[0.0]
        metrics=TransferMetrics([c],clock=lambda:clock[0],wall_clock=lambda:1000+clock[0])
        self.assertIsNone(metrics.sample())
        c.tx_bytes=4096;c.rx_bytes=1024;clock[0]=2
        metrics.record_rtt(7.5);result=metrics.sample()
        self.assertEqual(result['txBytesPerSecond'],2048)
        self.assertEqual(result['rxBytesPerSecond'],512)
        self.assertEqual(result['rttMs'],7.5)
        clock[0]=8;result=metrics.sample()
        self.assertEqual(result['txBytesPerSecond'],0)
        self.assertEqual(result['rxBytesPerSecond'],0)
        self.assertIsNone(result['rttMs'])
        metrics.record_rtt(float('nan'));self.assertIsNone(metrics.sample())

class QueueTests(unittest.TestCase):
    def test_burst_backpressure(self):
        units=BoundedUnits();units.put(b'a');units.put(b'b')
        def drain():
            time.sleep(0.02)
            value=units.get();units.release(value)
        worker=threading.Thread(target=drain);worker.start()
        units.put(b'c');worker.join()
        self.assertEqual(units.queue.qsize(),2)

    def test_congestion_and_memory_budget(self):
        units=BoundedUnits();units.put(b'a');units.put(b'b')
        with self.assertRaises(TransportError):units.put(b'c',timeout=0.001)
        with self.assertRaises(TransportError):units.put(bytes(4*1024*1024+1),timeout=0)


class WireTests(unittest.TestCase):
    def test_cpp_golden_vector(self):
        frame = Frame(VIDEO, bytes(0x003fffff), bytes(range(16)), 0x01020304, 0x0102030405060708)
        expected = bytes.fromhex('4446564401000080003fffff000102030405060708090a0b0c0d0e0f010203040102030405060708')
        self.assertEqual(frame.encode()[:40], expected)

    def test_all_fragment_boundaries(self):
        frame = Frame(PING, b'{"nonce":"a"}')
        wire = frame.encode()
        for split in range(len(wire)+1):
            reader = Reader('control')
            self.assertEqual(reader.feed(wire[:split]) + reader.feed(wire[split:]), [frame])
            reader.finish()

    def test_coalesced(self):
        a, b = Frame(PING, sequence=1), Frame(PING, sequence=2)
        self.assertEqual(Reader('control').feed(a.encode()+b.encode()), [a,b])

    def test_header_rejection_and_poisoning(self):
        for offset in (0,4,5,6,8):
            data = bytearray(Frame(PING).encode())
            data[offset] = 255
            reader = Reader('control')
            with self.assertRaises(TransportError): reader.feed(data)
            with self.assertRaises(TransportError): reader.feed(b'')

    def test_truncated(self):
        reader=Reader('control');reader.feed(Frame(PING,b'123').encode()[:-1])
        with self.assertRaises(TransportError):reader.finish()

    def test_replay_and_channel_isolation(self):
        reader=Reader('video');reader.feed(Frame(BIND).encode())
        with self.assertRaises(TransportError):reader.feed(Frame(BIND,sequence=2).encode())
        reader=Reader('control');reader.feed(Frame(PING).encode())
        with self.assertRaises(TransportError):reader.feed(Frame(PING).encode())
        with self.assertRaises(TransportError):Reader('control').feed(Frame(VIDEO).encode())

    def test_limits(self):
        with self.assertRaises(TransportError): Reader('control').feed(bytes(65537))
        with self.assertRaises(TransportError): Frame(PING,bytes(65537)).encode()
        with self.assertRaises(TransportError): Frame(PING,sequence=0).encode()


class JsonTests(unittest.TestCase):
    def test_duplicate_and_nonfinite(self):
        for value in (b'{"a":1,"a":2}',b'{"n":NaN}',b'{"n":1e999}',b'{"n":Infinity}',b'[]',b'\xff'):
            with self.assertRaises(TransportError): decode_json(value)

    def test_complexity(self):
        for value in ({'v':'a'*4097}, {'v':'\0'}, {'v':[0]*1025}):
            with self.assertRaises(TransportError):decode_json(json.dumps(value).encode())
        value={}
        for _ in range(18):value={'v':value}
        with self.assertRaises(TransportError):decode_json(json.dumps(value).encode())

    def test_schema(self):
        self.assertEqual(require_fields({'ready':True},('ready',)),{'ready':True})
        for value in ({},{'ready':True,'unknown':1}):
            with self.assertRaises(TransportError):require_fields(value,('ready',))


class BindingTests(unittest.TestCase):
    def setUp(self):
        self.now=10
        self.session=bytes(range(16));self.pin=bytes(range(32))
        self.tokens=BindingTokens(self.session,self.pin,1,clock=lambda:self.now)

    def test_one_use_and_revocation(self):
        token=self.tokens.tokens['video']
        self.tokens.consume('video',token,self.session,self.pin,1)
        with self.assertRaises(TransportError):self.tokens.consume('video',token,self.session,self.pin,1)
        token=self.tokens.tokens['input'];self.tokens.revoke()
        with self.assertRaises(TransportError):self.tokens.consume('input',token,self.session,self.pin,1)

    def test_wrong_binding(self):
        token=self.tokens.tokens['video']
        variants=[('input',token,self.session,self.pin,1),('video',token,bytes(16),self.pin,1),
                  ('video',token,self.session,bytes(32),1),('video',token,self.session,self.pin,2),
                  ('video','bad',self.session,self.pin,1)]
        for args in variants:
            with self.assertRaises(TransportError):self.tokens.consume(*args)
        self.tokens.consume('video',token,self.session,self.pin,1)

    def test_expiry(self):
        token=self.tokens.tokens['video'];self.now=20
        with self.assertRaises(TransportError):self.tokens.consume('video',token,self.session,self.pin,1)


class H264Tests(unittest.TestCase):
    def test_fragmented_aud(self):
        a=b'\0\0\0\1\x09\x10\0\0\1\x67\x11\0\0\1\x68\x22\0\0\1\x65\x33'
        b=b'\0\0\1\x09\x10\0\0\1\x41\x44'
        for size in (1,2,3,5,17,100):
            splitter=AccessUnitSplitter();result=[];wire=a+b
            for offset in range(0,len(wire),size):result+=splitter.feed(wire[offset:offset+size])
            result+=splitter.finish();self.assertEqual(result,[a,b])
            self.assertEqual(nal_types(a),[9,7,8,5])

    def test_invalid_headers(self):
        for data in (b'bad',b'\0\0\1',b'\0\0\1\xff',b'\0\0\1\x00',b'\0\0\1\x65\0\0\1'):
            with self.assertRaises(ValueError):nal_types(data)

    def test_nal_budget(self):
        with self.assertRaises(ValueError):nal_types(b'\0\0\1\x65'*257)

    def test_size_budget(self):
        splitter=AccessUnitSplitter()
        splitter.feed(b'\0\0\0\1\x09\x10')
        with self.assertRaises(ValueError):
            for _ in range(MAX_AU//65536+1):splitter.feed(b'a'*65536)


@unittest.skipUnless(ssl.HAS_TLSv1_3,'TLS 1.3 runtime required')
class TlsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='deskflow-display-tls-')
        cls.root=Path(cls.temp.name)
        for name in ('host','receiver','stranger'):init_identity(cls.root/name)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def exchange(self, client_name='receiver'):
        server=tls_context(self.root/'host',self.root/'receiver/cert.pem',True)
        client=tls_context(self.root/client_name,self.root/'host/cert.pem',False)
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(5)
        port=listener.getsockname()[1];errors=[]
        def accept():
            try:
                wire,_=listener.accept()
                secure=authenticate(server,wire,self.root/'receiver/cert.pem',True)
                try:
                    self.assertEqual(secure.recv(4),b'ping')
                    secure.sendall(b'pong')
                finally:secure.close()
            except Exception as error:errors.append(error)
            finally:listener.close()
        thread=threading.Thread(target=accept,daemon=True);thread.start()
        secure=None;success=False
        try:
            secure=authenticate(client,socket.create_connection(('127.0.0.1',port),timeout=4),self.root/'host/cert.pem')
            secure.sendall(b'ping');success=secure.recv(4)==b'pong'
        except (ssl.SSLError,OSError,TransportError):pass
        finally:
            if secure:secure.close()
            thread.join(timeout=6)
        self.assertFalse(thread.is_alive())
        return success,errors

    def test_mutual_tls13(self):
        success,errors=self.exchange();self.assertTrue(success);self.assertEqual(errors,[])

    def test_untrusted_client_rejected(self):
        success,errors=self.exchange('stranger');self.assertFalse(success);self.assertTrue(errors)

    def test_identity_permissions_and_no_overwrite(self):
        self.assertEqual((self.root/'host/key.pem').stat().st_mode & 0o777,0o600)
        before=fingerprint(self.root/'host/cert.pem')
        with self.assertRaises(FileExistsError):init_identity(self.root/'host')
        self.assertEqual(fingerprint(self.root/'host/cert.pem'),before)


if __name__=='__main__':unittest.main()
