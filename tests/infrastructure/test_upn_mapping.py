"""UPN additions must not disappear from the existing AR final-delivery report."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location("verify",ROOT/"scripts/verify_architecture_refactor.py")
verify=importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)

class NativeMappingTests(unittest.TestCase):
    def test_partial_native_contract_cannot_pass_from_its_implemented_subset(self):
        case={"id":"AR-19.a","stage":"R6","required":True,"runner":"ctest","test_names":["speex-real"],
              "remaining_contracts":["missing dependency and unsupported flush diagnostics"]}
        manifest={"stages":[f"R{i}" for i in range(7)],"requirements":[case]}
        row=verify.evaluate_cases(manifest,{"speex-real"},{"speex-real":"passed"},"R6")[0]
        self.assertEqual(row["status"],"not_verified")
        self.assertIn("missing dependency",row["reason"])
    def test_exact_six_and_document_binding(self):
        manifest=json.loads((ROOT/"tests/architecture_refactor_manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(verify.validate_upn_manifest(manifest),[])
        for mutation in ("missing","duplicate","hash"):
            broken=copy.deepcopy(manifest)
            block=broken["user_native_plugins"]
            if mutation=="missing": block["requirements"].pop()
            if mutation=="duplicate": block["requirements"].append(copy.deepcopy(block["requirements"][0]))
            if mutation=="hash": block["acceptance_document_sha256"]="0"*64
            self.assertTrue(verify.validate_upn_manifest(broken))

    def test_failed_missing_skipped_and_external_gaps_never_pass(self):
        manifest=json.loads((ROOT/"tests/architecture_refactor_manifest.json").read_text(encoding="utf-8"))
        name="UPN_external_sdk_pipeline"
        for state in ("failed","skipped","not_run","timeout"):
            rows=verify.evaluate_upn_cases(manifest,{name},{name:state},"R6")
            self.assertEqual(len(rows),6)
            self.assertTrue(all(r["status"]!="passed" for r in rows))
        self.assertTrue(all(r["status"]=="not_run" for r in verify.evaluate_upn_cases(manifest,set(),{},"R6")))
        rows=verify.evaluate_upn_cases(manifest,{name},{name:"passed"},"R6")
        self.assertEqual({r["id"] for r in rows if r["status"]=="passed"},{"UPN-01","UPN-04","UPN-05"})
        self.assertEqual({r["id"] for r in rows if r["status"]=="not_verified"},{"UPN-02","UPN-03","UPN-06"})
        self.assertEqual(verify.evaluate_upn_cases(manifest,{name},{name:"passed"},"R5"),[])

if __name__=="__main__": unittest.main()
