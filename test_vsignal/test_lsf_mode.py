#!/usr/bin/env python3
"""LSF proxy smoke test for the NPI-backed vsignal worker."""
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VSIGNAL = ROOT / "release/bin/vsignal"


def run(env, *args, ok=True):
    process = subprocess.run([str(VSIGNAL), *map(str, args), "--json"], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             universal_newlines=True, timeout=60)
    if ok and process.returncode:
        raise AssertionError(process.stderr)
    if ok:
        return json.loads(process.stdout)
    return process


def main():
    if not VSIGNAL.exists():
        raise SystemExit("vsignal is missing; run make vsignal first")
    with tempfile.TemporaryDirectory(prefix="vsignal-lsf-") as tmp:
        root = Path(tmp)
        worker = root / "remote_worker.py"
        worker.write_text(r'''#!/usr/bin/env python3
import json
import os
import sys

print(json.dumps({"type": "ready", "protocol": "vsignal-stdio-loop",
                  "version": 1, "pid": os.getpid(), "backend": "npi"},
                 separators=(',', ':')), flush=True)
for line in sys.stdin:
    if not line.strip():
        continue
    request = json.loads(line)
    command = request.get("cmd", "")
    response = {"id": request.get("id", 0), "status": "ok", "data": {}}
    if command == "status":
        response["data"] = {"design_source": "remote-kdb",
                             "uptime_seconds": 0, "pid": os.getpid()}
    print(json.dumps(response, separators=(',', ':')), flush=True)
    if command == "shutdown":
        break
''')
        worker.chmod(worker.stat().st_mode | stat.S_IEXEC)

        bsub = root / "bsub"
        bsub.write_text(r'''#!/usr/bin/env python3
import os
import sys

print('Job <6262> is submitted to queue <interactive>.', file=sys.stderr, flush=True)
args = sys.argv[1:]
for index, value in enumerate(args):
    if os.path.basename(value) in ('vsignal-worker', 'vsignal-worker-2022',
                                   'vsignal-worker-2024', 'vsignal-worker-2026'):
        record = os.environ.get('VSIGNAL_TEST_WORKER_RECORD')
        if record:
            with open(record, 'w') as handle:
                handle.write(os.path.basename(value))
        remote = os.environ['VSIGNAL_TEST_REMOTE_WORKER']
        os.execv(remote, [remote] + args[index + 1:])
raise SystemExit('worker command not found')
''')
        bsub.chmod(bsub.stat().st_mode | stat.S_IEXEC)

        for release, expected_worker in (("2022", "vsignal-worker-2022"),
                                         ("2024", "vsignal-worker-2024"),
                                         ("2026", "vsignal-worker-2026")):
            run_dir = root / ("run-" + release)
            record = root / ("worker-" + release)
            env = dict(os.environ, VERDI_HOME="/opt/Synopsys/verdi/" + release,
                       VWAVE_RUN_MODE="lsf", VWAVE_LSF_BSUB=str(bsub),
                       VWAVE_LSF_BKILL="/bin/true",
                       VSIGNAL_TEST_REMOTE_WORKER=str(worker),
                       VSIGNAL_TEST_WORKER_RECORD=str(record))
            opened = run(env, "open", "-dbdir", root / "remote-kdb",
                         "--run-dir", run_dir, "--timeout", "30")
            assert opened["status"] == "ok"
            assert record.read_text() == expected_worker
            status = run(env, "status", "--run-dir", run_dir)
            assert status["data"]["run_mode"] == "lsf"
            assert status["data"]["job_id"] == "6262"
            closed = run(env, "close", "--run-dir", run_dir)
            assert closed["status"] == "ok"
    print("vsignal LSF proxy smoke test passed")


if __name__ == "__main__":
    main()
