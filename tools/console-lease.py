#!/usr/bin/env python3
"""Coordinate console experiments between local sessions using an OS-held lock."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.0.190")
    parser.add_argument("--owner", default=str(Path.cwd()))
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("status")
    note = sub.add_parser("note")
    note.add_argument("message")
    run = sub.add_parser("run")
    run.add_argument("--wait", action="store_true", help="queue for the next console turn")
    run.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.host or any(c not in "0123456789abcdefABCDEF.:-" for c in args.host):
        parser.error("host must be an IP address")
    root = Path.home() / ".cache/eglantineps5-console" / args.host
    root.mkdir(parents=True, exist_ok=True)
    notices = []
    if (root / "messages.jsonl").exists():
        for line in (root / "messages.jsonl").read_text().splitlines()[-12:]:
            try:
                notice = json.loads(line)
            except json.JSONDecodeError:
                continue
            if notice.get("owner") != args.owner:
                notices.append(notice)
        notices = notices[-2:]
    if args.action == "note":
        with (root / "messages.jsonl").open("a") as messages:
            fcntl.flock(messages, fcntl.LOCK_EX)
            messages.write(json.dumps(dict(time=time.time(), owner=args.owner,
                                           message=args.message)) + "\n")
        return 0
    with (root / "console.lock").open("a+") as lock:
        # A queued waiter holds this second OS lock until it gets the console.
        # New non-waiting runs cannot repeatedly take the console ahead of it.
        with (root / "turn.lock").open("a+") as turn:
            if args.action == "run":
                try:
                    fcntl.flock(turn, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    if not args.wait:
                        print(json.dumps(dict(busy=True, reason="another session is queued", notices=notices)))
                        return 75
                    print(json.dumps(dict(queued=True, owner=args.owner)), flush=True)
                    fcntl.flock(turn, fcntl.LOCK_EX)
            announced = False
            while True:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if args.action == "status" or not args.wait:
                        print(json.dumps(dict(held=True, owner=(root / "owner.json").read_text()
                                              if (root / "owner.json").exists() else None, notices=notices)))
                        return 0 if args.action == "status" else 75
                    if not announced:
                        print(json.dumps(dict(queued=True, owner=args.owner)), flush=True)
                        announced = True
                    time.sleep(0.2)
        if args.action == "status":
            print(json.dumps(dict(held=False, directory=str(root), notices=notices)))
            return 0
        command = args.command
        if command[:1] == ["--"]:
            command = command[1:]
        if not command:
            parser.error("run requires a command")
        owner = root / "owner.json"
        owner.write_text(json.dumps(dict(pid=os.getpid(), owner=args.owner,
                                         started=time.time(), command=command), indent=2))
        if notices:
            print(json.dumps(dict(console_lease_notices=notices)), flush=True)
        try:
            # Keep the lease held by the experiment if its orchestrator exits.
            # subprocess.call kills its child when Ctrl-C interrupts wait(),
            # which can cut off the experiment's restoration/finally handler.
            with subprocess.Popen(command, pass_fds=(lock.fileno(),)) as child:
                try:
                    return child.wait()
                except KeyboardInterrupt:
                    # Terminal SIGINT already reaches the child process group.
                    # Keep the lease while that child performs its cleanup.
                    return child.wait()
        finally:
            owner.unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())
