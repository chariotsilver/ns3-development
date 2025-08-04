#!/usr/bin/env python3
"""
SKR-Optimizing ML Bridge for NS-3 QKD Controller

Purpose: Hill-climbing policy that maximizes Secret Key Rate (SKR) by adjusting pZ.
Usage: python ml_bridge.py

The ML bridge receives observation JSON from NS-3 and responds with action JSON.
Observation format: {"src": int, "dst": int, "nXX": int, "nZZ": int, "qberX": float, 
                     "keyBuf": int, "pZtx": float, "lastBits": int, "winSec": float}
Action format: {"pZ": float}

This policy optimizes SKR (bits/sec) using a simple hill-climber on pZ.
If M1 fails, lastBits=0, naturally pushing search away from too-high pZ.
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
    print(f"Starting SKR-optimizing ML bridge server on localhost:{port}")
    
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
        
        # per-session state: { "pz": float, "best_skr": float, "step": float }
        states = {}
        
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
                    
                    # Per-session state tracking
                    sid = f"{obs.get('src',0)}->{obs.get('dst',1)}"
                    st = states.setdefault(sid, {"pz": obs.get("pZtx", 0.9), "best_skr": -1.0, "step": 0.05})
                    
                    # Extract SKR signals
                    win_bits = obs.get("lastBits", 0)
                    win_sec = max(1e-6, obs.get("winSec", 0.1))
                    skr = win_bits / win_sec  # bits per second
                    
                    cur_pz, cur_best, step = st["pz"], st["best_skr"], st["step"]
                    
                    # Hill-climbing SKR optimization with per-session state
                    if skr > cur_best + 1e-9:          # improved: keep direction & (optionally) grow step a bit
                        cur_best = skr
                        step = min(0.2, step * 1.05)   # gentle expansion (optional)
                        # keep step sign as-is → continue in same direction
                        print(f"[{sid}] SKR ↑ to {skr:.1f} bps; step→{step:.3f}")
                    else:                               # no improvement: flip direction and shrink
                        step = -max(0.01, 0.5 * abs(step))
                        print(f"[{sid}] SKR {skr:.1f} bps; flip dir, step→{step:.3f}")
                    
                    # Calculate new pZ
                    new_pz = max(0.05, min(0.95, cur_pz + step))
                    states[sid].update({"pz": new_pz, "best_skr": max(cur_best, skr), "step": step})
                    
                    # Create action response
                    action = {"pZ": new_pz}
                    
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
