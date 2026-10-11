import json
import tempfile
import unittest
from pathlib import Path

from prefetch_agent.protocol import ProtocolError, load_experiments, load_targets, resolve_variables
from prefetch_agent.runner import make_plan


class ProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.targets = load_targets()
        cls.experiments = load_experiments()

    def test_registry_loads(self):
        self.assertEqual(set(self.targets), {"cortex-a725-cpu4", "cortex-x925-cpu6"})
        self.assertIn("region.existence", self.experiments)
        self.assertIn("calibration.cache-latency", self.experiments)

    def test_all_sources_exist(self):
        repo = Path(__file__).resolve().parent.parent
        for experiment in self.experiments.values():
            for source in experiment["sources"]:
                self.assertTrue((repo / source).is_file(), source)

    def test_plan_renders_without_shell(self):
        experiment = self.experiments["region.existence"]
        target = self.targets["cortex-x925-cpu6"]
        variables = resolve_variables(experiment, ["rounds=200", "access=store"])
        with tempfile.TemporaryDirectory() as directory:
            plan = make_plan(experiment, target, variables, Path(directory))
        self.assertEqual(plan["command"]["argv"][0], "python3")
        self.assertIn("200", plan["command"]["argv"])
        self.assertIn("store", plan["command"]["argv"])
        self.assertEqual(plan["missing_capabilities"], [])

    def test_invalid_override_rejected(self):
        with self.assertRaises(ProtocolError):
            resolve_variables(self.experiments["bop.existence"], ["train_pairs=99999"])

    def test_documents_are_json_objects(self):
        root = Path(__file__).resolve().parent.parent / "protocol"
        for path in root.rglob("*.json"):
            self.assertIsInstance(json.loads(path.read_text()), dict)


if __name__ == "__main__":
    unittest.main()
