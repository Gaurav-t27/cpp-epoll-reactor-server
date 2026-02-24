#!/usr/bin/env python3
"""
Comprehensive test suite for the C++ NIO Network Reactor TCP Server
Tests edge cases, partial reads/writes, concurrency, and error handling
"""

import socket
import time
import threading
import unittest
import subprocess
import random
import string
import signal
import sys
import os


class TestReactorServer(unittest.TestCase):
    """Base test case with server lifecycle management"""
    
    HOST = "127.0.0.1"
    PORT = 8080
    server_proc = None
    
    @classmethod
    def setUpClass(cls):
        """Start the C++ server before running tests"""
        server_path = os.path.join(os.path.dirname(__file__), "..", "build", "bin", "tcp_server")

        if not os.path.exists(server_path):
            raise FileNotFoundError(f"Server executable not found at {server_path}. Please build first.")

        cls.server_proc = subprocess.Popen(
            [server_path, str(cls.PORT)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )

        # Poll until the port accepts connections rather than sleeping a fixed amount
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if cls.server_proc.poll() is not None:
                raise RuntimeError("Server failed to start")
            try:
                with socket.create_connection((cls.HOST, cls.PORT), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            cls.server_proc.kill()
            cls.server_proc.wait()
            raise RuntimeError("Server did not become ready within 5 seconds")
    
    @classmethod
    def tearDownClass(cls):
        """Stop the server after all tests"""
        if cls.server_proc:
            cls.server_proc.send_signal(signal.SIGINT)
            try:
                cls.server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                cls.server_proc.kill()
                cls.server_proc.wait()

    def test_single_client_echo(self):
        """Test basic echo functionality with single client"""
        with socket.create_connection((self.HOST, self.PORT), timeout=2) as sock:
            msg = b"hello world"
            sock.sendall(msg)
            data = sock.recv(1024)
            print (f"Received: {data}")
            self.assertEqual(data, msg.upper())

    def test_1mb_payload(self):
        """Test sending 1MB of data"""
        large_data = b"x" * (1024 * 1024)
        with socket.create_connection((self.HOST, self.PORT), timeout=10) as sock:
            sock.sendall(large_data)
            
            received = b""
            while len(received) < len(large_data):
                chunk = sock.recv(8192)
                if not chunk:
                    break
                received += chunk
            
            print (f"Received {len(received)} bytes")
            self.assertEqual(len(received), len(large_data))
            self.assertEqual(received, large_data.upper())

    def test_partial_sends(self):
        """Test sending data in small chunks"""
        with socket.create_connection((self.HOST, self.PORT), timeout=5) as sock:
            msg = b"abcdefghijklmnopqrstuvwxyz"
            
            # Send one byte at a time
            for byte in msg:
                sock.send(bytes([byte]))
                time.sleep(0.01)  # Small delay between sends
            
            # Receive the echoed data
            received = b""
            while len(received) < len(msg):
                chunk = sock.recv(1)
                if not chunk:
                    break
                received += chunk
            
            print (f"Received: {received}")
            self.assertEqual(received, msg.upper())

    def test_100_concurrent_clients(self):
        """Test 100 clients concurrently — all must succeed"""
        results = []
        errors = []

        def client_task(client_id):
            try:
                with socket.create_connection((self.HOST, self.PORT), timeout=10) as sock:
                    msg = f"client{client_id}".encode()
                    sock.sendall(msg)
                    data = sock.recv(1024)
                    results.append(data == msg.upper())
            except Exception as e:
                results.append(False)
                errors.append(f"Client {client_id}: {e}")

        threads = [threading.Thread(target=client_task, args=(i,)) for i in range(100)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertEqual(len(results), 100, "Not all client threads completed")
        failures = results.count(False)
        self.assertEqual(failures, 0, f"{failures} clients failed:\n" + "\n".join(errors))

    def test_rapid_connect_disconnect(self):
        """Test rapid connection/disconnection cycles — server must stay alive"""
        for _ in range(100):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(1)
                sock.connect((self.HOST, self.PORT))
                sock.close()
            except OSError:
                pass

        self.assertIsNone(
            self.server_proc.poll(),
            "Server crashed during rapid connect/disconnect"
        )


    def test_backpressure(self):
        """Slow reader: send 128KB then read slowly — server must not crash or drop data"""
        data = b"a" * (128 * 1024)  # 128KB, twice the 64KB pause threshold

        with socket.create_connection((self.HOST, self.PORT), timeout=15) as sock:
            sock.sendall(data)

            received = b""
            sock.settimeout(10)
            while len(received) < len(data):
                chunk = sock.recv(1024)
                if not chunk:
                    break
                received += chunk
                time.sleep(0.001)  # Slow reader to trigger backpressure

            self.assertEqual(len(received), len(data), "Did not receive all bytes")
            self.assertEqual(received, data.upper(), "Data was not uppercased correctly")

        self.assertIsNone(self.server_proc.poll(), "Server crashed during backpressure test")


if __name__ == "__main__":
    unittest.main(verbosity=2)
