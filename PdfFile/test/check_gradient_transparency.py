"""Validate PDF soft-mask groups; requires qpdf on PATH.

Usage: python3 check_gradient_transparency.py document.pdf [minimum_masks]
The paired gradient_transparency.cpp fixture must contain four masks on two pages.
"""
import json
import subprocess
import sys

report = json.loads(subprocess.check_output(["qpdf", "--json", sys.argv[1]]))
objects = report["qpdf"][1]


def resolve(value):
    if isinstance(value, str) and "obj:" + value in objects:
        obj = objects["obj:" + value]
        return obj.get("value", obj.get("stream", {}).get("dict"))
    return value


masks = 0
for obj in objects.values():
    state = obj.get("value", {})
    if not isinstance(state, dict) or not isinstance(state.get("/SMask"), (str, dict)):
        continue
    mask = resolve(state["/SMask"])
    if not isinstance(mask, dict):
        continue
    form = resolve(mask.get("/G"))
    assert isinstance(form, dict) and form.get("/Subtype") == "/Form", "soft mask requires a form"
    group = resolve(form.get("/Group"))
    assert isinstance(group, dict), "soft-mask form is missing its transparency group"
    assert group.get("/S") == "/Transparency", "soft-mask group must be a transparency group"
    assert group.get("/CS") == "/DeviceRGB", "luminosity mask needs its blending colour space"
    masks += 1
assert masks >= int(sys.argv[2] if len(sys.argv) > 2 else 1), "expected transparent gradients"
print(f"PASS: {masks} valid soft masks across {len(report['pages'])} pages")
