"""Q8 consumes only root-validated counters and receipt metadata."""
import evidence_contract as c
from evidence_pipeline import CaptureSession


def capture_pair(request, pair, *, _session=CaptureSession, require_pairing=True):
    session = _session(request).start()
    try:
        pair()
    finally:
        receipt = session.stop()
    c.receipt(receipt, request['request'])
    c.need(receipt['status'] == 'FINALIZED', 'Q8_CAPTURE_FAILED')
    if require_pairing:
        counts = receipt['counts']
        c.need(counts['pairing_requests'] >= 1 and counts['pairing_responses'] >= 1
               and counts['security_requests'] == 0, 'Q8_SECURITY_ORDER_FAILED')
    return receipt
