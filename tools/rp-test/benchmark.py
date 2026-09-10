"""Three warm repetitions of each host-only CLI; JSON to stdout, no report file."""
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time

from common import InfraError, cli_error, need, verify_marker

def main():
    verify_marker()
    source = Path(__file__).resolve().parent
    results = {}
    commands = {
        'doctor': ['doctor.py', '--json'],
        'host-only': ['preflight.py', '--profile', 'host-only', '--json'],
        'reference': ['preflight.py', '--profile', 'reference', '--no-hardware', '--json'],
    }
    for label, command in commands.items():
        samples, phase_samples = [], []
        # Explicit unmeasured warmup, then three process-inclusive measurements.
        for repetition in range(4):
            start = time.monotonic()
            cp = subprocess.run([sys.executable, str(source / command[0]), *command[1:]], capture_output=True, text=True, timeout=65)
            elapsed = time.monotonic() - start
            need(cp.returncode == 0, 'BENCHMARK_PREFLIGHT_FAILED')
            value = json.loads(cp.stdout)
            need(value['ok'] is True and value['hardware_operations'] == 0, 'BENCHMARK_INVALID')
            if repetition:
                samples.append(elapsed)
                phase_samples.append(value['timing_ms'])
        threshold = 10 if label == 'doctor' else 60
        results[label] = {'min_seconds': min(samples), 'median_seconds': statistics.median(samples), 'max_seconds': max(samples),
                          'samples_seconds': samples, 'phase_ms': phase_samples, 'target_met': max(samples) <= threshold}
    print(json.dumps({'schema': 1, 'benchmarks': results, 'hardware_operations': 0}, sort_keys=True))
    return 0 if all(v['target_met'] for v in results.values()) else 2

if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        sys.exit(cli_error(exc))
