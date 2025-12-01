import csv
import re
import sys
import statistics
import argparse
import os

def analyze_logs(filename):
    # Data structures
    # Key: (Src, Dst, Slot, MsgID) -> Value: Start Timestamp
    packet_start_times = {} 
    latencies = []

    # Regex to extract packet info: "PKT 0->4 (S:1 M:1)"
    # Group 1: Src, Group 2: Dst, Group 3: Slot, Group 4: MsgID
    packet_pattern = re.compile(r"PKT (\d+)->(\d+) \(S:(\d+) M:(\d+)\)")

    print(f"Analyzing {filename}...\n")

    try:
        with open(filename, 'r') as csvfile:
            reader = csv.reader(csvfile)
            
            for row in reader:
                # Skip empty lines or malformed rows
                if not row or len(row) < 4: continue
                
                try:
                    timestamp = float(row[0])
                    log_type = row[2]
                    message = row[3]
                except ValueError:
                    continue # Skip header or bad numbers

                # Look for the packet pattern in the message column
                match = packet_pattern.search(message)
                if match:
                    src = int(match.group(1))
                    dst = int(match.group(2))
                    slot = int(match.group(3))
                    msg_id = int(match.group(4))
                    
                    # Create a unique key for this specific packet instance
                    packet_key = (src, dst, slot, msg_id)

                    if log_type == "SEND":
                        # Record start time (Origination)
                        packet_start_times[packet_key] = timestamp
                        
                    elif log_type == "RECV":
                        # Calculate latency if we saw the start
                        if packet_key in packet_start_times:
                            start_time = packet_start_times.pop(packet_key)
                            latency_ms = (timestamp - start_time) * 1000.0 # Convert to ms
                            
                            latencies.append({
                                "packet": packet_key,
                                "latency": latency_ms,
                                "hops": abs(dst - src) # Simple hop calc for line/matrix topology
                            })
                            
    except FileNotFoundError:
        print(f"Error: Could not find file '{filename}'")
        sys.exit(1)

    # --- Reporting ---

    print(f"{'Packet ID (Src->Dst S:M)':<30} | {'Hops':<5} | {'Latency (ms)':<15}")
    print("-" * 60)

    for item in latencies:
        key = item['packet']
        pkt_str = f"{key[0]}->{key[1]} (Slot:{key[2]} Msg:{key[3]})"
        print(f"{pkt_str:<30} | {item['hops']:<5} | {item['latency']:.3f} ms")

    print("-" * 60)
    print(f"Total Packets Analyzed: {len(latencies)}")

    if latencies:
        all_values = [x['latency'] for x in latencies]
        avg_lat = statistics.mean(all_values)
        min_lat = min(all_values)
        max_lat = max(all_values)
        
        print(f"\nStatistics:")
        print(f"  Average Latency: {avg_lat:.3f} ms")
        print(f"  Min Latency:     {min_lat:.3f} ms")
        print(f"  Max Latency:     {max_lat:.3f} ms")
    else:
        print("\nNo complete packet traversals found (SEND -> RECV match).")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Analyze network latency from relay logs.")
    parser.add_argument("file", nargs="?", default="./logs/merged_log.csv", help="Path to the merged log CSV file")
    args = parser.parse_args()

    analyze_logs(args.file)