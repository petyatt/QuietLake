import json
import unittest
from pathlib import Path

from pike_env import Encounter
from train_pike import forward


class PikeTrainingTests(unittest.TestCase):
    def test_missed_strike_costs_energy_without_feeding(self):
        env = Encounter(42)
        env.distance = 8
        energy, hunger = env.energy, env.hunger
        reward, _, caught, cost = env.step(2)
        self.assertFalse(caught)
        self.assertLess(reward, 0)
        self.assertAlmostEqual(env.energy, energy - cost)
        self.assertEqual(env.hunger, hunger)

    def test_policy_probabilities(self):
        model = json.loads(Path(__file__).with_name("pike_policy.json").read_text())
        for seed in range(50):
            _, probabilities = forward(model, Encounter(seed).observe())
            self.assertEqual(len(probabilities), 4)
            self.assertTrue(all(0 <= p <= 1 for p in probabilities))
            self.assertAlmostEqual(sum(probabilities), 1)


if __name__ == "__main__":
    unittest.main()
