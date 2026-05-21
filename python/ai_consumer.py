#!/usr/bin/env python3
"""Consume AI engine results over a Unix domain socket.

Wire format (big-endian integers), one framed message per result:

    [u32 total_len][u32 json_len][json][full jpeg][crop jpeg...]

The JSON carries every size so the trailing binary blob is sliced
deterministically: the full-frame JPEG first, then one crop JPEG per
detection that has cropJpegSize > 0, in detection order.

This is where Python "owns the decision": it matches face embeddings against
a gallery and then persists the full + crop images for events worth keeping.
"""

import argparse
import datetime
import json
import os
import pickle
import signal
import socket
import struct
import sys

RUNNING = True


def on_signal(signum, frame):
    del signum, frame
    global RUNNING
    RUNNING = False


def recv_exact(sock, n):
    """Read exactly n bytes or return None when the peer closes."""
    chunks = []
    got = 0
    while got < n:
        chunk = sock.recv(n - got)
        if not chunk:
            return None
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


def recv_message(sock):
    """Return (meta_dict, full_jpeg_bytes, [crop_jpeg_bytes]) or None on close."""
    header = recv_exact(sock, 4)
    if header is None:
        return None
    total_len = struct.unpack(">I", header)[0]
    body = recv_exact(sock, total_len)
    if body is None:
        return None

    json_len = struct.unpack(">I", body[:4])[0]
    meta = json.loads(body[4:4 + json_len].decode("utf-8"))
    blob = body[4 + json_len:]

    offset = 0
    full_size = int(meta.get("fullJpegSize", 0))
    full_jpeg = blob[offset:offset + full_size]
    offset += full_size

    crops = []
    for det in meta.get("detections", []):
        size = int(det.get("cropJpegSize", 0))
        if size > 0:
            crops.append(blob[offset:offset + size])
            offset += size
        else:
            crops.append(b"")
    return meta, full_jpeg, crops


def load_face_db(path):
    """Load {'entries': [{'name', 'embedding', ...}]} and return [(name, vec)]."""
    import numpy as np

    with open(path, "rb") as fp:
        obj = pickle.load(fp)
    raw = obj["entries"] if isinstance(obj, dict) and "entries" in obj else obj

    entries = []
    for idx, item in enumerate(raw):
        if isinstance(item, dict):
            name = item.get("name") or item.get("label") or f"person_{idx}"
            emb = item.get("embedding")
        elif isinstance(item, (list, tuple)) and len(item) >= 2:
            name, emb = item[0], item[1]
        else:
            continue
        if emb is None:
            continue
        vec = np.asarray(emb, dtype=np.float32).reshape(-1)
        norm = float(np.linalg.norm(vec))
        if norm > 1e-9:
            entries.append((str(name), vec / norm))
    return entries


def match_face(embedding, entries):
    """Return (best_name, best_score) using cosine similarity."""
    import numpy as np

    vec = np.asarray(embedding, dtype=np.float32).reshape(-1)
    norm = float(np.linalg.norm(vec))
    if norm <= 1e-9 or not entries:
        return None, 0.0
    vec = vec / norm
    best_name, best_score = None, -1.0
    for name, ref in entries:
        score = float(np.dot(vec, ref))
        if score > best_score:
            best_name, best_score = name, score
    return best_name, best_score


def save_event(save_dir, meta, full_jpeg, crops):
    """Persist the full frame and per-detection crops for one result."""
    date = datetime.date.today().isoformat()
    folder = os.path.join(save_dir, str(meta["cameraId"]), date)
    os.makedirs(folder, exist_ok=True)
    seq = meta["seq"]
    stem = os.path.join(folder, f"{seq:010d}")

    saved = []
    if full_jpeg:
        full_path = f"{stem}_full.jpg"
        with open(full_path, "wb") as fp:
            fp.write(full_jpeg)
        saved.append(full_path)
    for i, crop in enumerate(crops):
        if crop:
            crop_path = f"{stem}_det{i}.jpg"
            with open(crop_path, "wb") as fp:
                fp.write(crop)
            saved.append(crop_path)
    return saved


def show_debug(meta, full_jpeg, entries, match_threshold):
    """Decode the full frame and draw detections on it for visual debugging.

    Boxes and keypoints are in full-resolution coordinates and full_jpeg is the
    full-resolution frame, so they overlay 1:1 with no scaling. Needs a display
    (run on the desktop, or over SSH with X forwarding).
    """
    import cv2
    import numpy as np
    import time

    # Measure how often results arrive (= the AI processing rate per second).
    st = show_debug.__dict__
    st["count"] = st.get("count", 0) + 1
    now = time.time()
    if "t0" not in st:
        st["t0"], st["fps"] = now, 0.0
    elif now - st["t0"] >= 1.0:
        st["fps"] = st["count"] / (now - st["t0"])
        st["count"], st["t0"] = 0, now

    if not full_jpeg:
        return
    img = cv2.imdecode(np.frombuffer(full_jpeg, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return

    for det in meta.get("detections", []):
        x1, y1 = int(det["x1"]), int(det["y1"])
        x2, y2 = int(det["x2"]), int(det["y2"])

        label = f"cls{det['classId']} {det['score']:.2f}"
        color = (0, 255, 0)
        embedding = det.get("embedding") or []
        if embedding and entries:
            name, score = match_face(embedding, entries)
            if name is not None and score >= match_threshold:
                label = f"{name} {score:.2f}"
            else:
                label = f"unknown {score:.2f}"
                color = (0, 165, 255)

        cv2.rectangle(img, (x1, y1), (x2, y2), color, 2)
        cv2.putText(img, label, (x1, max(14, y1 - 6)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv2.LINE_AA)

        kps = det.get("keypoints", [])  # flat (x, y, score) triples
        for k in range(0, len(kps) - 2, 3):
            if kps[k + 2] > 0:
                cv2.circle(img, (int(kps[k]), int(kps[k + 1])),
                           3, (0, 0, 255), -1)

    footer = (f"cam={meta['cameraId']} job={meta['jobId']} "
              f"seq={meta['seq']} det={len(meta.get('detections', []))} "
              f"fps={st['fps']:.1f}")
    cv2.putText(img, footer, (8, img.shape[0] - 8),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 255), 1, cv2.LINE_AA)

    cv2.imshow(f"AI debug: cam {meta['cameraId']}", img)
    cv2.waitKey(1)


def parse_args():
    parser = argparse.ArgumentParser(description="AI engine result consumer")
    parser.add_argument("--socket", default="/tmp/ai_engine.sock",
                        help="Unix socket path the AI engine listens on")
    parser.add_argument("--db", default="/home/orangepi/Documents/test/face_database_rknn.pkl",
                        help="Face embedding pickle for recognition (optional)")
    parser.add_argument("--match-threshold", type=float, default=0.4,
                        help="Minimum cosine similarity to accept a face match")
    parser.add_argument("--save", choices=["none", "all", "matched"],
                        default="all",
                        help="When to persist full + crop images")
    parser.add_argument("--save-dir", default="events",
                        help="Root directory for saved images")
    parser.add_argument("--show", action="store_true",
                        help="Show the full frame with detections drawn "
                             "(needs a display)")
    return parser.parse_args()


def main():
    args = parse_args()
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    entries = []
    if args.db:
        try:
            entries = load_face_db(args.db)
            print(f"Loaded {len(entries)} face embeddings from {args.db}")
        except Exception as exc:  # noqa: BLE001
            print(f"Could not load face DB: {exc}", file=sys.stderr)

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(args.socket)
    except OSError as exc:
        print(f"Cannot connect to {args.socket}: {exc}", file=sys.stderr)
        return 1
    print(f"Connected to AI engine at {args.socket}")

    while RUNNING:
        try:
            message = recv_message(sock)
        except OSError:
            break
        if message is None:
            print("AI engine closed the connection", file=sys.stderr)
            break

        meta, full_jpeg, crops = message
        detections = meta.get("detections", [])
        any_match = False

        for i, det in enumerate(detections):
            label = f"class={det['classId']}"
            embedding = det.get("embedding") or []
            if embedding and entries:
                name, score = match_face(embedding, entries)
                if name is not None and score >= args.match_threshold:
                    print(name,score,meta['cameraId'])

            #         label = f"{name} ({score:.3f})"
            #         any_match = True
            #     else:
            #         label = f"unknown ({score:.3f})"
            # print(f"  cam={meta['cameraId']} job={meta['jobId']} "
            #       f"seq={meta['seq']} det={i} "
            #       f"score={det['score']:.3f} box=({det['x1']:.0f},{det['y1']:.0f},"
            #       f"{det['x2']:.0f},{det['y2']:.0f}) {label}")

            # should_save = (args.save == "all" and detections) or \
            #             (args.save == "matched" and any_match)
            # if should_save:
            #     saved = save_event(args.save_dir, meta, full_jpeg, crops)
            #     if saved:
            #         print(f"  saved {len(saved)} image(s) -> {os.path.dirname(saved[0])}")

        if args.show:
            show_debug(meta, full_jpeg, entries, args.match_threshold)

    sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
