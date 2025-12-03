import csv
import re
import sys
import statistics
import argparse
import os

def analyze_logs(filename, out_csv):
    # Data structures
    # Key: (Src, Dst, Slot, MsgID) -> Value: {ts, observer, payload}
    packet_start_times = {}
    latencies = []

    # Per-node lists
    per_sender = {}
    per_dest = {}

    # Regex to extract packet info: "PKT 0->4 (S:1 M:1)"
    packet_pattern = re.compile(r"PKT\s+(\d+)->(\d+)\s*\(S:(\d+)\s+M:(\d+)\)", re.IGNORECASE)

    try:
        with open(filename, 'r', newline='') as csvfile:
            reader = csv.reader(csvfile)
            for row in reader:
                if not row or len(row) < 4:
                    continue

                # parse basic fields with defensive checks
                try:
                    timestamp = float(row[0])
                except Exception:
                    continue
                observer = None
                try:
                    observer = int(row[1])
                except Exception:
                    observer = row[1]
                event = row[2].upper()
                message = row[3]

                match = packet_pattern.search(message)
                if not match:
                    continue

                src = int(match.group(1))
                dst = int(match.group(2))
                slot = int(match.group(3))
                msg_id = int(match.group(4))
                key = (src, dst, slot, msg_id)

                if event == "SEND":
                    # keep earliest send timestamp for a key
                    existing = packet_start_times.get(key)
                    if existing is None or timestamp < existing["ts"]:
                        packet_start_times[key] = {"ts": timestamp, "observer": observer, "payload": message}
                elif event == "RECV":
                    start = packet_start_times.pop(key, None)
                    if start is None:
                        # unmatched recv; skip
                        continue
                    send_ts = start["ts"]
                    recv_ts = timestamp
                    latency_ms = (recv_ts - send_ts) * 1000.0
                    entry = {
                        "sender": src,
                        "dest": dst,
                        "slot": slot,
                        "msg_id": msg_id,
                        "send_time": send_ts,
                        "recv_time": recv_ts,
                        "latency_ms": latency_ms,
                        "send_observer": start["observer"],
                        "recv_observer": observer,
                        "send_payload": start["payload"],
                        "recv_payload": message
                    }
                    latencies.append(entry)

                    # accumulate per-sender and per-dest
                    per_sender.setdefault(src, []).append(latency_ms)
                    per_dest.setdefault(dst, []).append(latency_ms)

    except FileNotFoundError:
        print(f"Error: Could not find file '{filename}'", file=sys.stderr)
        sys.exit(1)

    # write matched rows to CSV for visualization
    if out_csv:
        try:
            with open(out_csv, 'w', newline='') as outfh:
                writer = csv.writer(outfh)
                writer.writerow([
                    "sender","dest","slot","msg_id",
                    "send_time","recv_time","latency_ms",
                    "send_observer","recv_observer",
                    "send_payload","recv_payload"
                ])
                for e in latencies:
                    writer.writerow([
                        e["sender"], e["dest"], e["slot"], e["msg_id"],
                        f"{e['send_time']:.6f}", f"{e['recv_time']:.6f}", f"{e['latency_ms']:.3f}",
                        e["send_observer"], e["recv_observer"],
                        e["send_payload"], e["recv_payload"]
                    ])
            print(f"Wrote {len(latencies)} matched packets to {out_csv}")
        except Exception as exc:
            print(f"Failed to write CSV {out_csv}: {exc}", file=sys.stderr)

    # Console reporting
    print(f"\nTotal matched packets: {len(latencies)}")
    if latencies:
        all_vals = [e["latency_ms"] for e in latencies]
        print(f"Overall latency ms: min={min(all_vals):.3f} mean={statistics.mean(all_vals):.3f} median={statistics.median(all_vals):.3f} max={max(all_vals):.3f}")
    else:
        print("No matched SEND->RECV pairs found.")

    # Per-sender summary
    if per_sender:
        print("\nPer-sender latency summary (ms):")
        print(f"{'Sender':>6} {'count':>6} {'mean':>8} {'median':>8} {'min':>8} {'max':>8} {'stdev':>8}")
        for sender in sorted(per_sender.keys()):
            vals = per_sender[sender]
            cnt = len(vals)
            mean = statistics.mean(vals)
            med = statistics.median(vals)
            mn = min(vals)
            mx = max(vals)
            sd = statistics.stdev(vals) if cnt > 1 else 0.0
            print(f"{sender:6d} {cnt:6d} {mean:8.3f} {med:8.3f} {mn:8.3f} {mx:8.3f} {sd:8.3f}")

    # Per-destination summary
    if per_dest:
        print("\nPer-destination latency summary (ms):")
        print(f"{'Dest':>6} {'count':>6} {'mean':>8} {'median':>8} {'min':>8} {'max':>8} {'stdev':>8}")
        for dest in sorted(per_dest.keys()):
            vals = per_dest[dest]
            cnt = len(vals)
            mean = statistics.mean(vals)
            med = statistics.median(vals)
            mn = min(vals)
            mx = max(vals)
            sd = statistics.stdev(vals) if cnt > 1 else 0.0
            print(f"{dest:6d} {cnt:6d} {mean:8.3f} {med:8.3f} {mn:8.3f} {mx:8.3f} {sd:8.3f}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Analyze log CSV to extract latencies between SEND and RECV events.")
    parser.add_argument("file", nargs="?", default="./logs/merged_log.csv", help="Path to the merged log CSV file")
    parser.add_argument("-o", "--out", default="latencies.csv", help="Output CSV file for matched latencies")
    args = parser.parse_args()

    analyze_logs(args.file, args.out)