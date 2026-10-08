#!/usr/bin/env python3
"""Builder step 2, bucket: strip, ambient effects only. Thin wrapper over curate.py."""
import runpy, sys
sys.argv = ["curate.py", "strip", "--ambient", *sys.argv[1:]]
runpy.run_path(__file__.replace("curate-strip-ambient.py", "curate.py"), run_name="__main__")
