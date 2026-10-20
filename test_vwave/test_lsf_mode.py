#!/usr/bin/env python3
"""LSF proxy smoke test using a local bsub-compatible fake."""
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VWAVE = ROOT / "release/bin/vwave"
FSDB = ROOT / "test_vwave/tb_top.fsdb"
SDK = Path(os.environ.get("FFR_SDK_HOME", "/opt/Synopsys/verdi/T-2022.06-SP2"))


def run(env, *args, ok=True):
    p = subprocess.run([str(VWAVE), *map(str, args), "--json"], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True, timeout=60)
    if ok and p.returncode:
        raise AssertionError(p.stderr)
    if ok:
        return json.loads(p.stdout)
    return p


def main():
    if not (VWAVE.exists() and FSDB.exists()):
        raise SystemExit("vwave or fixture is missing")
    with tempfile.TemporaryDirectory(prefix="vwave-lsf-") as tmp:
        fake = Path(tmp) / "bsub"
        fake.write_text("""#!/usr/bin/env python3
import os, sys
print('Job <4242> is submitted to queue <interactive>.', file=sys.stderr, flush=True)
a = sys.argv[1:]
while a:
    if a[0] in ('-I', '-Is', '-Ip'):
        a = a[1:]
    elif a[0] in ('-J', '-q', '-R'):
        a = a[2:]
    elif 'vwave-' in a[0] and a[0].endswith('-worker'):
        os.execv(a[0], a)
    else:
        a = a[1:]
raise SystemExit('worker command not found')
""")
        fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
        run_dir = Path(tmp) / "run"
        env = dict(os.environ, VWAVE_RUN_MODE="lsf", VWAVE_LSF_BSUB=str(fake),
                   VWAVE_LSF_BKILL="/bin/true", VWAVE_LSF_QUEUE="interactive")
        run(env, "open", FSDB, "--backend", "ffr", "--verdi-home", SDK,
            "--run-dir", run_dir, "--timeout", "30")
        status = run(env, "status", "--run-dir", run_dir)
        assert status["data"]["backend"] == "ffr"
        assert status["data"]["run_mode"] == "lsf"
        assert status["data"]["job_id"] == "4242"
        value = run(env, "get", "-s", "tb.clk", "-t", "1000", "--run-dir", run_dir)
        assert value["data"]["values"][0]["value"] == "0"
        local_env = dict(env, VWAVE_RUN_MODE="local")
        switched = run(local_env, "open", FSDB, "--backend", "ffr", "--verdi-home", SDK,
                       "--run-dir", run_dir, "--timeout", "30")
        assert switched["status"] == "ok"
        local_status = run(local_env, "status", "--run-dir", run_dir)
        assert local_status["data"].get("run_mode") != "lsf"
        run(local_env, "close", "--run-dir", run_dir)
    print("LSF proxy smoke test passed")


if __name__ == "__main__":
    main()
