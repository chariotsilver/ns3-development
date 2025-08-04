#!/usr/bin/env python3
"""
Minimal Python ML Bridge for NS-3 QKD Bias Controller

Purpose: Quick test policy that targets 10% X-ratio by nudging pZ.
Usage: python ml_bridge.py

The ML bridge receives observation JSON from NS-3 and responds with action JSON.
Observation format: {"nXX": int, "nZZ": int, "pZtx": float, "sessionId": string}
Action format: {"pZ": float}

This toy policy pushes pZ toward 1 - rX_target (since rX ~ (1-pZ))
"""

import json
import socket
import threading
import sys

def serve(port=5557):
    """
    Serve ML bridge on localhost:port
    
    Args:
        port (int): TCP port to listen on (default 5557)
    """
    print(f"Starting ML bridge server on localhost:{port}")
    
    # Create and configure socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        s.bind(("127.0.0.1", port))
        s.listen(1)
        print(f"ML bridge listening on port {port}...")
        
        # Accept connection from NS-3
        conn, addr = s.accept()
        print(f"ML bridge connected to {addr}")
        
        buf = b""
        observation_count = 0
        
        while True:
            # Receive data from NS-3
            data = conn.recv(4096)
            if not data:
                print("NS-3 connection closed")
                break
                
            buf += data
            
            # Process complete JSON lines
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                
                try:
                    # Parse observation from NS-3
                    obs = json.loads(line.decode())
                    observation_count += 1
                    
                    print(f"Observation {observation_count}: {obs}")
                    
                    # Extract key metrics
                    n_total = max(1, obs.get("nXX", 0) + obs.get("nZZ", 0))
                    rX_actual = obs.get("nXX", 0) / n_total
                    pZ_current = obs.get("pZtx", 0.8)
                    session_id = obs.get("sessionId", "unknown")
                    
                    # Toy ML policy: target 10% X-ratio
                    rX_target = 0.10
                    
                    # Since rX ~ (1-pZ), we adjust pZ to reach target
                    # Use proportional control with gain 0.08
                    gain = 0.08
                    error = rX_target - rX_actual
                    pZ_new = pZ_current - gain * error
                    
                    # Clamp pZ to safe bounds
                    pZ_new = max(0.05, min(0.95, pZ_new))
                    
                    # Create action response
                    action = {"pZ": pZ_new}
                    
                    print(f"  Session {session_id}: rX={rX_actual:.4f} target={rX_target:.4f} pZ: {pZ_current:.4f}->{pZ_new:.4f}")
                    
                    # Send action back to NS-3
                    response = json.dumps(action).encode() + b"\n"
                    conn.sendall(response)
                    
                except json.JSONDecodeError as e:
                    print(f"JSON decode error: {e}")
                    continue
                except Exception as e:
                    print(f"Error processing observation: {e}")
                    continue
                    
    except KeyboardInterrupt:
        print("\nML bridge shutting down...")
    except Exception as e:
        print(f"Server error: {e}")
    finally:
        try:
            conn.close()
        except:
            pass
        s.close()

if __name__ == "__main__":
    # Allow custom port via command line
    port = 5557
    if len(sys.argv) > 1:
        try:
            port = int(sys.argv[1])
        except ValueError:
            print(f"Invalid port: {sys.argv[1]}, using default {port}")
    
    serve(port)
