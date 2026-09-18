#!/usr/bin/env python3
"""Resume recorded terminal metadata only. Never rerun qualification phases."""
import sys
import evidence_contract as c
from evidence_pipeline import resume_terminal
if __name__ == '__main__':
    c.need(len(sys.argv)==2,'RUN_ID_REQUIRED')
    print(c.encode(resume_terminal(sys.argv[1])).decode(),end='')
