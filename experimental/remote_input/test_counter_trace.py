"""Frozen synthetic behavioral oracle from unmodified merged main f43c224."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("candidate", type=Path)
parser.add_argument("--baseline", type=Path)
args = parser.parse_args()

root = Path(__file__).resolve().parents[2]
abi = (root / "sdk/include/robotweax/srt_telemetry.h").read_text()
normalized = (Path(__file__).parent / "include/numeric_input_draft.hpp").read_text()
for domain, struct, count in [("raw", "RawValues", 27), ("ext", "ExtendedValues", 26)]:
    body = re.search(
        r"typedef struct robotweax_srt_telemetry_" + domain + r"_sample_v1\s*\{(.*?)\}",
        abi, re.S,
    ).group(1)
    metadata = {"available_fields", "capabilities", "observed_at_unix_nano", "sample_monotonic_nano"}
    fields = [(kind, name) for kind, name in re.findall(r"\b(uint64_t|double)\s+(\w+)\s*;", body)
              if name not in metadata]
    # Match the whole struct, rather than stopping at a scalar {} initializer.
    declaration = re.search(r"^struct " + struct + r"\s*\{(.*?)^\};", normalized, re.S | re.M).group(1)
    assert len(fields) == count
    for kind, name in fields:
        assert re.search(r"\b(?:std::)?" + kind + r"\s+" + name + r"\b", declaration), (struct, name)
    assert "observed_at_unix_nano" not in declaration and "sample_monotonic_nano" not in declaration
    catalog = re.findall(r"RWX_NUMERIC_FIELD\((\w+), (\w+), (\w+)\)",
                         (root / f"sdk/src/{domain}_numeric_fields.inc").read_text())
    assert [(kind, name) for kind, name, bit in catalog] == fields
    assert all(bit in abi for kind, name, bit in catalog)

output = subprocess.check_output([str(args.candidate.resolve())])
reference = json.loads((Path(__file__).parent / "counter-trace-baseline.json").read_text())
assert len(output.splitlines()) == reference["operations"]
assert hashlib.sha256(output).hexdigest() == reference["sha256"], "existing C API counter behavior changed"
if args.baseline:
    baseline = subprocess.check_output([str(args.baseline.resolve())])
    assert output == baseline, "candidate and explicitly supplied baseline outputs differ"
print("PASS 53 normalized fields and 2048 legacy C API raw/ext/provider/clock/reset/availability "
      "operations match f43c224 exactly")
