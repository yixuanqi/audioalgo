"""One real external consumer, shared by UPN-01..06. Never rebuild the host.

The controlled install/source/build/package directories are outside the repository.
Only the SDK install and its shipped template enter the consumer project. All
commands, catalogs, WAVs, hashes and child JUnit remain in the report directory.
"""
from __future__ import annotations
import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_wave(path, channels, rate):
    frames = len(channels[0])
    raw = b"".join(struct.pack("<f", channels[c][i]) for i in range(frames) for c in range(len(channels)))
    fmt = struct.pack("<HHIIHH", 3, len(channels), rate, rate*len(channels)*4, len(channels)*4, 32)
    payload = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(raw)) + raw
    path.write_bytes(b"RIFF" + struct.pack("<I", len(payload)+4) + b"WAVE" + payload)


def read_wave(path):
    raw = path.read_bytes()
    assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE"
    pos, fmt, data = 12, None, None
    while pos+8 <= len(raw):
        tag, size = raw[pos:pos+4], struct.unpack_from("<I", raw, pos+4)[0]
        value = raw[pos+8:pos+8+size]
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", value)
        if tag == b"data":
            data = value
        pos += 8+size+(size & 1)
    assert fmt and data is not None
    encoding, count, rate, _, _, bits = fmt
    assert encoding in (3, 0xfffe) and bits == 32, fmt
    samples = struct.unpack("<"+"f"*(len(data)//4), data)
    return rate, [samples[c::count] for c in range(count)]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    report = ROOT / "build/reports/loops/P1-native-sdk" / stamp
    report.mkdir(parents=True)
    external = Path(tempfile.mkdtemp(prefix="AudioAlgo-UPN-"))
    assert ROOT.resolve() not in external.resolve().parents
    env = os.environ.copy()
    env.update(HTTP_PROXY="http://127.0.0.1:9", HTTPS_PROXY="http://127.0.0.1:9",
               ALL_PROXY="http://127.0.0.1:9", NO_PROXY="", AUDIOALGO_PLUGIN_PATH=str(external / "empty"))
    records, artifacts = [], []
    summary = {"schema_version": 1, "scope": "UPN_automated_subset", "status": "failed",
               "external_root": str(external), "report_directory": str(report),
               "commands": records, "artifacts": artifacts,
               "limitations": ["Real desktop remains paused; this is component/CLI evidence, not desktop acceptance.",
                               "Network proxies disabled; no network download command or dependency is used. This is not an OS firewall test.",
                               "Windows x64 MSVC toolchain and installed Windows/MSVC CRT are declared system dependencies."]}

    def run(command, expected=0, extra=None):
        started = dt.datetime.now(dt.timezone.utc).isoformat()
        result = subprocess.run([str(s) for s in command], cwd=external, env=env | (extra or {}),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=150)
        log = report / f"{len(records):02d}.log"
        log.write_bytes(result.stdout)
        records.append({"argv": [str(s) for s in command], "cwd": str(external), "started_utc": started,
                        "ended_utc": dt.datetime.now(dt.timezone.utc).isoformat(), "exit_code": result.returncode,
                        "log_path": str(log), "log_sha256": sha(log)})
        assert result.returncode == expected, (command, result.returncode, log, result.stdout.decode(errors="replace")[-2500:])
        return result.stdout.decode("utf-8", errors="replace")

    def artifact(p):
        artifacts.append({"path": str(p), "sha256": sha(p)})

    try:
        build = args.build_dir.resolve()
        hosts = {"audio_cli.exe": build/"bin/audio_cli.exe",
                 "audio_native_plugin_tests.exe": build/"bin/audio_native_plugin_tests.exe"}
        editor = build/"AudioAlgoEditor_artefacts"/args.config/"AudioAlgo Editor.exe"
        if not editor.is_file():
            editor = build/"bin/AudioAlgo Editor.exe"
        if editor.is_file():
            hosts["AudioAlgo Editor.exe"] = editor
        host_hashes = {name: sha(path) for name, path in hosts.items()}
        host = external / "host"
        host.mkdir()
        for name, path in hosts.items():
            shutil.copy2(path, host/name)
        cli = host/"audio_cli.exe"
        sdk, source, binary, package = (external/n for n in ("sdk", "consumer", "consumer-build", "plugins/01-stateful"))
        run([args.cmake, "--install", build, "--prefix", sdk, "--component", "AudioAlgoNodeSDK", "--config", args.config])
        shutil.copytree(sdk/"share/AudioAlgoNodeSDK/native_stateful", source)
        assert {p.name for p in source.iterdir()} == {"CMakeLists.txt", "plugin.cpp", "manifest.json.in", "descriptor.h.in"}
        # Also compile the public header as C, independent of the C++ example.
        with (source/"CMakeLists.txt").open("a") as s:
            s.write('\nenable_language(C)\nadd_library(c_abi_check OBJECT abi_check.c)\nset_target_properties(c_abi_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)\ntarget_link_libraries(c_abi_check PRIVATE AudioAlgo::node_abi)\n')
        (source/"abi_check.c").write_text('#include <audioalgo/node_abi_v1.h>\n_Static_assert(sizeof(uint32_t)==4,"width");\nint probe(void){return AA_NODE_ABI_V1;}\n')

        def configure(revision):
            run([args.cmake, "-S", source, "-B", binary, "-G", "Visual Studio 18 2026", "-A", "x64",
                 f"-DCMAKE_PREFIX_PATH={sdk}", "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
                 "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF", f"-DPLUGIN_REVISION={revision}"])
            run([args.cmake, "--build", binary, "--config", args.config, "--parallel", "2"])
            run([args.cmake, "--install", binary, "--config", args.config, "--prefix", package])

        configure(1)
        shutil.copy2(binary/"CMakeCache.txt",report/"consumer-cache-v1.txt")
        shutil.copytree(package,report/"package-v1")
        for p in (report/"consumer-cache-v1.txt", source/"CMakeLists.txt", report/"package-v1/manifest.json", report/"package-v1/external_stateful.dll"):
            artifact(p)
        assert str(ROOT).lower() not in (binary/"CMakeCache.txt").read_text().lower()
        baseline_hash, baseline_manifest_hash = sha(package/"external_stateful.dll"), sha(package/"manifest.json")
        catalog = json.loads(run([cli, "--list-nodes", "--plugin-dir", external/"plugins"]))
        assert catalog["diagnostics"] == []
        assert len([n for n in catalog["nodes"] if n["type_id"] == "example.external_stateful"]) == 1

        # Same genuine plugin package used for all rejection tests. Failures cannot
        # leave nodes behind, and unrelated builtins must remain usable.
        negative = external/"negative"
        negative.mkdir()
        manifest = json.loads((package/"manifest.json").read_text())
        variants = {"abi": ("plugin_abi_incompatible", {"abi_version":2}),
                    "architecture": ("plugin_platform_incompatible", {"architecture":"arm64"}),
                    "missing": ("plugin_library_missing", {"library":"absent.dll"}),
                    "escape": ("plugin_manifest_invalid", {"library":"../escape.dll"}),
                    "mismatch": ("plugin_descriptor_mismatch", {"version":"99.0.0"})}
        for name, (code, change) in variants.items():
            p = negative/name
            shutil.copytree(package,p)
            (p/"manifest.json").write_text(json.dumps(manifest | change))
            c = json.loads(run([cli,"--list-nodes","--plugin-dir",p],1))
            assert [e["code"] for e in c["diagnostics"]] == [code], c
            assert all(n["type_id"] != "example.external_stateful" for n in c["nodes"])
            assert any(n["type_id"] == "builtin.gain" for n in c["nodes"])
        p = negative/"entry"
        shutil.copytree(package,p)
        shutil.copy2(Path(os.environ["WINDIR"])/"System32/version.dll",p/"external_stateful.dll")
        c = json.loads(run([cli,"--list-nodes","--plugin-dir",p],1))
        assert [e["code"] for e in c["diagnostics"]] == ["plugin_entry_missing"], c
        duplicates = external/"duplicates"
        shutil.copytree(package,duplicates/"01-good")
        shutil.copytree(package,duplicates/"02-duplicate")
        c = json.loads(run([cli,"--list-nodes","--plugin-dir",duplicates],1))
        assert [e["code"] for e in c["diagnostics"]] == ["plugin_id_duplicate"]
        assert len([n for n in c["nodes"] if n["type_id"] == "example.external_stateful"]) == 1

        audio = external/"audio"
        audio.mkdir()
        a = [[f32(0.12*math.sin((i+1)*(c+1)*0.037)) for i in range(513)] for c in range(2)]
        b = [[f32(0.07*math.cos((i+3)*(c+1)*0.021)) for i in range(513)] for c in range(2)]
        for name, signal in (("a",a),("b",b)):
            write_wave(audio/f"{name}.wav",signal,48000)
        run([host/"audio_native_plugin_tests.exe","--reporter","junit","--out",report/"component-junit.xml"],extra={
            "AUDIOALGO_UPN_PACKAGE":str(package),"AUDIOALGO_UPN_OUTPUT":str(audio)})
        junit = ET.parse(report/"component-junit.xml").getroot()
        assert len(junit.findall(".//testcase")) == 2
        assert not junit.findall(".//failure") and not junit.findall(".//skipped")
        assert json.loads((audio/"component-catalog.json").read_text()) == catalog
        artifact(report/"component-junit.xml")
        artifact(audio/"main.aaproj"); artifact(audio/"side.aaproj")
        errors = {}

        def check_audio(folder, port, revision):
            rate, outputs = read_wave(folder/"output.wav")
            assert rate == 48000 and len(outputs) == 2 and all(len(x)==513 for x in outputs)
            maximum = 0
            for c in range(2):
                for i in range(513):
                    first = f32(revision*1.5*a[c][i]+b[c][i]+0.00001*i)
                    expected = revision*0.5*first+b[c][i]-0.00002*i if port=="main" else first-b[c][i]
                    maximum = max(maximum,abs(outputs[c][i]-expected))
            assert maximum < 1e-6, (folder, maximum)
            run_manifest=json.loads((folder/"run.json").read_text())
            plugins=run_manifest["plugins"]
            assert {p["node_id"] for p in plugins} == {"first","second"}
            assert all(p["library_sha256"]==sha(package/"external_stateful.dll") and
                       p["manifest_sha256"]==sha(package/"manifest.json") and
                       p["plugin_version"]==f"1.0.{revision}" for p in plugins)
            assert (folder/"complete.json").is_file()
            artifact(folder/"output.wav");artifact(folder/"run.json")
            errors[str(folder)] = maximum

        for port in ("main","side"):
            check_audio(audio/f"component-{port}",port,1)
            for block in (1,127,512):
                output=audio/f"cli-{port}-{block}"
                run([cli,"run",audio/f"{port}.aaproj",output,block,"--plugin-dir",package])
                check_audio(output,port,1)
        # Rejections do not poison the process's builtin registry or successful audio.
        project=json.loads((audio/"main.aaproj").read_text())
        project["nodes"]=[project["nodes"][0],{"id":"gain","type_id":"builtin.gain","parameters":{"gain_db":0.0}}]
        project["connections"]=[{"from_node":"a","from_port":"audio","to_node":"gain","to_port":"audio_in"}]
        project["selected_output_node_id"]="gain";project["selected_output_port_id"]="audio_out"
        (audio/"builtin.aaproj").write_text(json.dumps(project))
        run([cli,"run",audio/"builtin.aaproj",audio/"builtin-recovery",127,"--plugin-dir",negative])
        _, recovered=read_wave(audio/"builtin-recovery/output.wav")
        assert max(abs(x-y) for c in range(2) for x,y in zip(recovered[c],a[c])) < 1e-6

        # Only the plugin project is rebuilt. All host processes have exited before
        # replacement. Subsequent CLI process discovers behavior and provenance v2.
        old_run=(audio/"cli-main-127/run.json").read_bytes()
        configure(2)
        shutil.copytree(package,report/"package-v2")
        artifact(report/"package-v2/manifest.json"); artifact(report/"package-v2/external_stateful.dll")
        assert sha(package/"external_stateful.dll") != baseline_hash
        assert sha(package/"manifest.json") != baseline_manifest_hash
        run([cli,"run",audio/"main.aaproj",audio/"updated",127,"--plugin-dir",package])
        check_audio(audio/"updated","main",2)
        assert (audio/"cli-main-127/run.json").read_bytes() == old_run
        assert {name:sha(path) for name,path in hosts.items()} == host_hashes
        assert {name:sha(host/name) for name in hosts} == host_hashes
        shutil.copytree(source,report/"consumer-project")
        shutil.copytree(audio,report/"audio")
        for p in sorted((report/"audio").rglob("*")):
            if p.is_file(): artifact(p)
        summary.update(status="passed",max_abs_errors=errors,host_sha256=host_hashes,
                       host_unchanged=True,baseline_library_sha256=baseline_hash,
                       updated_library_sha256=sha(package/"external_stateful.dll"),
                       native_dependency="Windows x64 + installed MSVC CRT; no JUCE or host library in plugin build",
                       requirement_status={f"UPN-{i:02}":"automated_subset_passed" for i in range(1,7)})
    finally:
        (report/"report.json").write_text(json.dumps(summary,indent=2)+"\n")
        print(f"UPN evidence: {report/'report.json'}",flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
