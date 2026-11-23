"""Headless pike encounters. Distances in metres, reserves normalised to 0..1."""
import math
import random

ACTIONS = ("wait", "approach", "strike", "retreat")
FEATURES = ("range_over_8m", "cover", "danger", "energy", "hunger")


class Encounter:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.distance = self.rng.uniform(0.4, 7)
        self.cover = self.rng.random()
        self.danger = self.rng.random()
        self.energy = self.rng.uniform(0.35, 1)
        self.hunger = self.rng.uniform(0.35, 1)
        self.steps = 0

    def observe(self):
        return [self.distance / 8, self.cover, self.danger, self.energy, self.hunger]

    def step(self, action):
        catch_roll, threat_roll, drift = [self.rng.random() for _ in range(3)]
        caught = False
        cost = (0.004, 0.025, 0.12, 0.035)[action]
        if action == 1:
            self.distance = max(0.25, self.distance - 1.1)
        elif action == 2:
            chance = math.exp(-0.48 * self.distance**2) * (0.55 + 0.45 * self.cover) * min(1, 2 * self.energy)
            caught = catch_roll < chance
        elif action == 3:
            self.danger *= 0.35
            self.cover = min(1, self.cover + 0.3)
            self.distance = min(8, self.distance + 0.6)
        injury = threat_roll < 0.18 * self.danger**2 * (1 - 0.6 * self.cover)
        self.energy = max(0, self.energy - cost)
        reward = -cost - 0.01 - 0.45 * injury
        if caught:
            reward += self.hunger
            self.hunger = max(0, self.hunger - 0.32)
        elif action == 2:
            reward -= 0.14
            self.distance += 1.3
        self.distance = min(8, max(0.25, self.distance + (drift - 0.53) * 0.8))
        self.steps += 1
        done = caught or injury or self.energy == 0 or self.steps >= 24
        return reward, done, caught, cost


def rules(x):
    distance, cover, danger, energy, hunger = x
    if danger > 0.78 and cover < 0.5:
        return 3
    return 2 if distance * 8 < 1.25 else 1
