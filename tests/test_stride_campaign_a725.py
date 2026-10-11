import unittest

from prefetch_agent.stride_campaign_a725 import parse_a725_args


class A725CampaignTests(unittest.TestCase):
    def test_a725_defaults(self):
        args = parse_a725_args(["--output", "/tmp/a725", "--arch", "A725"])
        self.assertEqual(args.arch, "A725")
        self.assertEqual(args.core, 4)
        self.assertEqual(args.hit_threshold, 120)

    def test_explicit_core_and_threshold_win(self):
        args = parse_a725_args([
            "--output", "/tmp/a725", "--arch", "A725",
            "--core", "10", "--hit-threshold", "99",
        ])
        self.assertEqual(args.core, 10)
        self.assertEqual(args.hit_threshold, 99)


if __name__ == "__main__":
    unittest.main()
