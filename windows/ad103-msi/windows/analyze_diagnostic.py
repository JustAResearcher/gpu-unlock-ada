#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only observations from complete 0.4.1 diagnostic ownership logs.

This is not a board-profile parser, authenticity check, or release admission
policy. Even a syntactically complete report never authorizes an unlock.
Incomplete/erroring metadata inventories are rejected rather than guessed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

MAX_BYTES = 128 * 1024
NUMBER = r"(?:0x[0-9a-f]+|[0-9]+)"
GUID = r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}"
DP = "09576e91-6d3f-11d2-8e39-00a0c969723b"
PCI = "4cf5b200-68b8-4ca5-9eec-b23e3f50029a"
GOP = "9042a9de-23dc-4a38-96fb-7aded080516a"
TEXT = "387477c2-69c7-11d2-8e39-00a0c969723b"
EDID = "1c0c34f6-d380-41fa-a049-8ad06c1a66aa"
ACTIVE = "bd8c1056-9f36-44ec-92a8-a6337f817986"
NAMES = {DP: "DEVICE_PATH", PCI: "PCI_IO", GOP: "GOP", TEXT: "SIMPLE_TEXT_OUT",
         "387477c1-69c7-11d2-8e39-00a0c969723b": "SIMPLE_TEXT_IN",
         EDID: "EDID_DISCOVERED", ACTIVE: "EDID_ACTIVE",
         "d3b36f2c-d551-11d4-9a46-0090273fc14d": "CONSOLE_OUT_DEVICE"}
PASSIVE = {1, 2, 4}
UNSUPPORTED = 0x8000000000000003
DENIED = 0x800000000000000f
ERROR_BIT = 1 << 63


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def number(value: str, maximum: int = (1 << 64) - 1) -> int:
    result = int(value, 16 if value.startswith("0x") else 10)
    require(result <= maximum, "numeric field exceeds its bound")
    return result


class Reader:
    def __init__(self, lines: list[str]):
        self.lines, self.index = lines, 0

    def peek(self) -> str:
        return self.lines[self.index] if self.index < len(self.lines) else ""

    def take(self, pattern: str) -> tuple[str, ...]:
        match = re.fullmatch(pattern, self.peek())
        require(match is not None, f"incomplete or unexpected ownership line {self.index + 1}")
        self.index += 1
        return match.groups()


def open_record(values: tuple[str, ...]) -> dict:
    attributes, count, agent, controller = (number(value) for value in values)
    require(attributes <= 0xffffffff and 0 < count <= 0xffffffff,
            "invalid open attributes/count")
    return dict(attributes=attributes, count=count, agent=agent, controller=controller)


OPEN_FIELDS = rf"attributes=({NUMBER}) count=([0-9]+) agent=({NUMBER}) controller=({NUMBER})"


def path_nodes(reader: Reader, prefix: str) -> list[dict]:
    nodes, offset = [], 0
    for index in range(128):
        raw = reader.take(rf"{prefix}\[{index}\] offset=([0-9]+) type=([0-9]+) subtype=([0-9]+) bytes=([0-9]+)")
        position, kind, subtype, size = (number(value) for value in raw)
        require(position == offset and kind <= 255 and subtype <= 255 and
                4 <= size <= 4096 - offset, "invalid or unbounded path node")
        nodes.append(dict(offset=position, type=kind, subtype=subtype, bytes=size))
        offset += size
        if kind == 127:
            require(subtype == 255 and size == 4, "invalid path end node")
            return nodes
    raise ValueError("path has no bounded end node")


def child_inventory(reader: Reader, record: dict) -> dict:
    raw = reader.take(rf"child-detail handle=({NUMBER}) console_out=([01]) console_in=([01]) stderr=([01]) observational_only=1")
    handle = number(raw[0])
    require(handle == record["controller"] and handle != 0, "child handle mismatch")
    child = dict(handle=handle, parent_record=record["index"],
                 parent_agent=record["agent"],
                 console={key: bool(int(value)) for key, value in zip(("out", "in", "stderr"), raw[1:])},
                 protocols=[], blocking_consumers=[], classification="unclassified")
    status, count = reader.take(rf"child-detail protocol_status=({NUMBER}) protocol_count=([0-9]+)")
    require(number(status) == 0 and 1 <= number(count) <= 32, "incomplete child protocol inventory")
    total, seen = 0, set()
    for index in range(number(count)):
        guid, = reader.take(rf"child-protocol\[{index}\] guid=({GUID})")
        require(guid not in seen, "duplicate child protocol GUID")
        seen.add(guid)
        logged_name, = reader.take(rf"child-protocol\[{index}\] name=([A-Z_]+)")
        expected_name = NAMES.get(guid, "OTHER")
        # The firmware logger knows fewer names than this offline decoder.
        logger_name = expected_name if expected_name in {
            "PCI_IO", "DEVICE_PATH", "GOP", "SIMPLE_TEXT_OUT", "SIMPLE_TEXT_IN"} else "OTHER"
        require(logged_name == logger_name, "protocol GUID and logged name disagree")
        status, count = reader.take(rf"child-open protocol_index={index} status=({NUMBER}) records=([0-9]+)")
        count = number(count)
        require(number(status) == 0 and count <= 32 and total + count <= 64,
                "incomplete or unbounded child open inventory")
        opens = [open_record(reader.take(rf"child-open\[{index}:{entry}\] {OPEN_FIELDS}"))
                 for entry in range(count)]
        total += count
        protocol = dict(index=index, guid=guid, name=expected_name, opens=opens)
        child["protocols"].append(protocol)
        for opened in opens:
            if opened["attributes"] not in PASSIVE:
                child["blocking_consumers"].append(dict(protocol=guid, name=expected_name, **opened))
    status, present = reader.take(rf"child-detail path_status=({NUMBER}) path_present=([01])")
    require(number(status) == 0 and present == "1" and DP in seen,
            "complete accessible child device path required")
    nodes = path_nodes(reader, "child-path")
    size, digest = reader.take(r"child-detail path_bytes=([0-9]+) path_sha256=([0-9a-f]{64})")
    require(number(size) == sum(node["bytes"] for node in nodes), "path size does not match nodes")
    child["path"] = dict(nodes=nodes, bytes=number(size), reported_sha256=digest,
                         hash_verified=False, resolved_parent=None, validated_pci_ancestry=False)
    child["inventory_complete"] = True
    child["edid_contents_captured"] = False
    child["physical_connectedness"] = "unknown"
    return child


def child_check(reader: Reader, child: dict, target: int) -> int:
    handle = child["handle"]
    raw = reader.take(rf"child-gop handle=({NUMBER}) status=({NUMBER}) present=([01]) mode_present=([01])")
    found, status = number(raw[0]), number(raw[1])
    present, mode = bool(int(raw[2])), bool(int(raw[3]))
    require(found == handle, "GOP lookup handle mismatch")
    child["gop"] = dict(status=status, present=present, mode_present=mode)
    reason, result = None, 0
    if status & ERROR_BIT:
        require(not mode, "failed GOP lookup cannot have a validated mode")
        reason, result = "gop-protocol", UNSUPPORTED
    else:
        require(status == 0 and present, "inconsistent GOP lookup result")
        require(any(p["guid"] == GOP for p in child["protocols"]), "GOP missing from inventory")
        if not mode:
            reason, result = "gop-mode", UNSUPPORTED
        else:
            raw = reader.take(rf"child-gop mode_info=([01]) info_bytes=([0-9]+) framebuffer=({NUMBER}) framebuffer_bytes=([0-9]+)")
            info, size, framebuffer, capacity = (number(value) for value in raw)
            child["gop"].update(mode_info=bool(info), info_bytes=size,
                                framebuffer=framebuffer, framebuffer_bytes=capacity)
            valid = bool(info and size >= 36 and framebuffer and capacity)
            if info and size >= 36:
                raw = reader.take(r"child-gop width=([0-9]+) height=([0-9]+) pixel_format=([0-9]+) pixels_per_scanline=([0-9]+)")
                width, height, pixel, stride = (number(value, 0xffffffff) for value in raw)
                child["gop"].update(width=width, height=height, pixel_format=pixel, pixels_per_scanline=stride)
                valid = valid and width > 0 and height > 0 and pixel <= 1 and stride >= width and stride * height <= capacity // 4
            if not valid:
                reason, result = "gop-geometry", UNSUPPORTED
            else:
                raw = reader.take(rf"child-path lookup_status=({NUMBER}) path_present=([01]) locate_available=([01])")
                status, present, locate = (number(value) for value in raw)
                if status & ERROR_BIT or not present or not locate:
                    reason, result = "device-path", UNSUPPORTED
                else:
                    require(status == 0, "unsupported path lookup warning")
                    checked = path_nodes(reader, "child-check-path")
                    require(checked == child["path"]["nodes"], "device path changed between logged inspections")
                    raw = reader.take(rf"child-parent locate_status=({NUMBER}) parent=({NUMBER}) target=({NUMBER})")
                    status, parent, actual_target = (number(value) for value in raw)
                    require(actual_target == target, "child-parent target mismatch")
                    child["path"].update(locate_status=status, resolved_parent=parent if status == 0 else None)
                    if status & ERROR_BIT or parent != target:
                        reason, result = "pci-parent", DENIED
                    else:
                        require(status == 0, "unsupported PCI ancestry warning")
                        child["path"]["validated_pci_ancestry"] = True
    if reason:
        raw = reader.take(rf"child-check handle=({NUMBER}) reason=([a-z-]+) status=({NUMBER})")
        require(number(raw[0]) == handle and raw[1] == reason and number(raw[2]) == result,
                "child refusal reason/status mismatch")
    child["refusal_reason"], child["status"] = reason, result
    return result


def analyze_report(data: bytes) -> dict:
    """Return observations; raise ValueError on unsupported/incomplete grammar."""
    require(isinstance(data, bytes) and 0 < len(data) <= MAX_BYTES, "report exceeds bounded input size")
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError("report must contain ASCII diagnostic text") from error
    text = text.replace("\r\n", "\n")
    require(text.endswith("\n") and all(c == "\n" or 32 <= ord(c) <= 126 for c in text),
            "report must have complete lines and no control characters")
    lines = text.splitlines()
    require(len(lines) <= 2048 and all(len(line) <= 2048 for line in lines), "report line bound exceeded")
    prefix = ["gpu-unlock-ada Windows preboot port 0.4.1", None,
              "PROFILE_TAG=diagnostic-only mode=diagnostic",
              "DIAGNOSTIC_ONLY: GPU writes, firmware execution, reset and driver detach are disabled."]
    require(len(lines) >= 7 and all(wanted is None or lines[i] == wanted for i, wanted in enumerate(prefix))
            and re.fullmatch(r"RUN[0-9]{2}", lines[1]) is not None, "expected a 0.4.1 diagnostic report")
    starts = [i for i, line in enumerate(lines) if line.startswith("ownership:")]
    require(len(starts) == 1, "expected exactly one ownership snapshot")
    start = starts[0]
    # Ignore hardware-specific preamble data, but not a hidden topology/final.
    reserved = ("owner", "child-", "display child:", "release ", "FINAL", "DIAGNOSTIC_COMPLETE",
                "CHECK stage=owner-observe", "gpu-unlock-ada ", "RUN", "PROFILE_TAG=", "DIAGNOSTIC_ONLY:")
    require(start >= 4 and not any(line.startswith(reserved) for line in lines[4:start]),
            "unexpected ownership or final data before snapshot")
    reader = Reader(lines[start:])
    count, exclusive, status = reader.take(rf"ownership: records=([0-9]+) exclusive=([01]) status=({NUMBER})")
    require(number(count) <= 32 and exclusive == "0" and number(status) == 0,
            "complete non-exclusive ownership snapshot required")
    records = []
    for index in range(number(count)):
        raw = reader.take(rf"owner\[{index}\] protocol=([0-9a-f]{{8}}) {OPEN_FIELDS}")
        guid, = reader.take(rf"owner-protocol\[{index}\] guid=({GUID})")
        require(guid[:8] == raw[0], "owner short/full protocol GUID mismatch")
        records.append(dict(index=index, protocol=guid, name=NAMES.get(guid, "OTHER"), **open_record(raw[1:])))
    owners = [r for r in records if r["protocol"] == PCI and r["attributes"] == 16]
    require(len(owners) == 1, "report does not establish one parent PCI_IO owner")
    owner = owners[0]
    require(owner["agent"] and owner["controller"] and owner["count"] == 1,
            "invalid parent PCI_IO ownership record")
    target, agent = owner["controller"], owner["agent"]
    children, failure, visited = [], 0, set()
    expected = [r for r in records if r["attributes"] == 8]
    for record in expected:
        if not reader.peek().startswith("child-detail handle="):
            break
        require(not failure and record["protocol"] in {PCI, DP} and record["agent"] == agent
                and record["count"] == 1 and record["controller"] not in {0, target, agent}
                and len(children) < 8 and record["controller"] not in visited,
                "unmatched, duplicate or out-of-order child relationship")
        child = child_inventory(reader, record)
        failure = child_check(reader, child, target)
        raw = reader.take(rf"display child: record=([0-9]+) handle=({NUMBER}) agent=({NUMBER}) status=({NUMBER})")
        require(tuple(number(value) for value in raw) == (record["index"], child["handle"], agent, failure),
                "display child summary does not match its parent relationship/check")
        visited.add(child["handle"])
        children.append(child)
    candidate = None
    if reader.peek().startswith("release candidate:"):
        require(not failure and len(children) == len(expected), "candidate after incomplete/refused children")
        raw = reader.take(rf"release candidate: agent=({NUMBER}) image=({NUMBER}) source=({NUMBER}) base=({NUMBER}) bytes=([0-9]+) version=({NUMBER}) code_type=([0-9]+) children=([0-9]+)")
        values = [number(value) for value in raw]
        require(values[0] == agent and values[-1] == len(children), "release candidate identity/count mismatch")
        candidate = dict(zip(("agent", "image", "source", "base", "bytes", "version", "code_type", "children"), values))
    if reader.peek().startswith("release refused:"):
        require(not failure, "duplicate release refusal")
        refused, = reader.take(r"release refused: different owner or unsupported open at record ([0-9]+)")
        require(number(refused) < len(records), "invalid refused owner index")
        failure = DENIED
    checked, = reader.take(rf"CHECK stage=owner-observe status=({NUMBER})")
    checked = number(checked)
    if checked == 0:
        require(not failure and candidate is not None and len(children) == len(expected), "incomplete successful owner observation")
        require(candidate["image"] and candidate["base"] and 0 < candidate["bytes"] <= 64 * 1024 * 1024
                and candidate["code_type"] == 3, "inconsistent successful driver-image observation")
        reader.take(r"DIAGNOSTIC_COMPLETE eligible=1 gpu_writes=0")
    else:
        require(checked & ERROR_BIT and (not failure or checked == failure), "owner refusal status mismatch")
    raw = reader.take(rf"FINAL mode=diagnostic status=({NUMBER}) stage=(owner-observe|complete) gpu_writes=0")
    require(number(raw[0]) == checked and raw[1] == ("complete" if checked == 0 else "owner-observe")
            and reader.index == len(reader.lines), "inconsistent final status, stage, or trailing report data")
    return observations(data, records, children, target, agent, candidate, checked, raw[1], len(expected))


def observations(data: bytes, records: list, children: list, target: int, agent: int,
                 candidate: dict | None, status: int, stage: str, expected: int) -> dict:
    blockers = [
        dict(code="unlock-not-authorized", message="Text observations do not authorize an unlock or driver release."),
        dict(code="descendant-inventory-incomplete", message="Downstream consumer handles and the complete console dependency graph were not inventoried."),
        dict(code="consumer-driver-identities-unverified", message="Consumer driver images, code identities and release behavior were not verified."),
        dict(code="unauthenticated-report", message="A local text report and its SHA-256 are not authenticated hardware evidence."),
    ]
    unknown = {r["protocol"] for r in records if r["name"] == "OTHER"}
    for child in children:
        protocols = {p["guid"] for p in child["protocols"]}
        unknown.update(p["guid"] for p in child["protocols"] if p["name"] == "OTHER")
        if child["status"] == 0 and child["path"]["validated_pci_ancestry"]:
            child["classification"] = "physical-gop-output"
        elif (protocols == {EDID, DP} and not child["blocking_consumers"]
              and child["gop"]["status"] == UNSUPPORTED
              and not child["gop"]["present"] and not child["gop"]["mode_present"]
              and child["refusal_reason"] == "gop-protocol"):
            child["classification"] = "passive-edid-output"
        if child["refusal_reason"] == "gop-protocol" and child["classification"] == "unclassified":
            blockers.append(dict(code="gop-absence-unestablished", handle=child["handle"],
                                 message="The GOP query and protocol inventory do not establish a passive EDID-only output."))
        if child["blocking_consumers"]:
            blockers.append(dict(code="active-downstream-consumers", handle=child["handle"],
                                 message="Child protocols have non-passive consumers/relationships; direct console equality cannot exclude console dependencies."))
        if not child["path"]["validated_pci_ancestry"]:
            blockers.append(dict(code="pci-ancestry-unverified", handle=child["handle"],
                                 message="The parent BY_CHILD relationship is recorded, but successful PCI ancestry resolution was not captured."))
    if unknown:
        blockers.append(dict(code="unknown-protocols", guids=sorted(unknown), message="Unidentified full protocol GUIDs are preserved without approval."))
    if len(children) != expected:
        blockers.append(dict(code="parent-child-inventory-incomplete", message="Some parent BY_CHILD relationships have no complete child observation."))
    if any(r["attributes"] not in PASSIVE | {8, 16} for r in records):
        blockers.append(dict(code="unsupported-parent-open", message="Parent ownership includes unsupported open attributes."))
    summary = [f"Observed {len(children)} of {expected} parent-linked children in a structurally complete 0.4.1 ownership report."]
    summary += [f"Child {c['handle']:#x}: {c['classification']}; {len(c['blocking_consumers'])} non-passive open records." for c in children]
    summary += ["EDID protocol presence does not identify a monitor, connector, cable state or EDID contents.",
                "The report does not establish complete downstream ownership or driver identities. Unlock is not authorized."]
    return dict(schema_version=1, report_version="0.4.1", source_sha256=hashlib.sha256(data).hexdigest(),
                ownership_syntax_complete=True, dependency_graph_complete=False, unlock_authorized=False,
                eligibility="not-established", final=dict(status=status, stage=stage, gpu_writes=0),
                target=dict(handle=target, owner_agent=agent), owner_records=records,
                release_candidate=candidate, children=children, blockers=blockers, summary=summary)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--json", action="store_true", help="print structured observations")
    args = parser.parse_args(argv)
    try:
        with args.report.open("rb") as stream:
            data = stream.read(MAX_BYTES + 1)
        result = analyze_report(data)
    except (OSError, ValueError) as error:
        print(f"Cannot analyze report: {error}. Unlock is not authorized.", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print("\n".join(result["summary"]))
        print(f"Diagnostic final: {result['final']['status']:#x} at {result['final']['stage']}; reported GPU writes: 0.")
        print("This program only reads the report; it changes no firmware, drivers, Windows settings or profiles.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
