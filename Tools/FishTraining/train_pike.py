"""Train a small neural policy with episodic policy gradients using standard Python."""
import argparse
import json
import math
import random
from pathlib import Path

from pike_env import ACTIONS, FEATURES, Encounter, rules


def forward(model, obs):
    hidden = [math.tanh(sum(a*b for a, b in zip(row, obs + [1]))) for row in model["hidden"]]
    logits = [sum(a*b for a, b in zip(row, hidden + [1])) for row in model["output"]]
    weights = [math.exp(v - max(logits)) for v in logits]
    return hidden, [w / sum(weights) for w in weights]


def train(episodes, seed):
    rng = random.Random(seed)
    def matrix(rows, cols):
        return [[rng.uniform(-0.35, 0.35) for _ in range(cols)] for _ in range(rows)]
    model = dict(version=1, features=FEATURES, actions=ACTIONS, hidden=matrix(12, 6), output=matrix(4, 13))
    baseline = 0.0
    for n in range(episodes):
        env = Encounter(rng.getrandbits(60))
        history = []
        while True:
            x = env.observe()
            h, p = forward(model, x)
            a = rng.choices(range(4), weights=p)[0]
            reward, done, _, _ = env.step(a)
            history.append((x, h, p, a, reward))
            if done:
                break
        future = 0
        rate = 0.016 / (1 + n / episodes)
        for x, h, p, a, reward in reversed(history):
            future = reward + 0.98 * future
            delta = [rate * (future - baseline) * (int(j == a) - prob) for j, prob in enumerate(p)]
            for i, row in enumerate(model["hidden"]):
                grad = (1 - h[i]**2) * sum(d * out[i] for d, out in zip(delta, model["output"]))
                for j, value in enumerate(x + [1]):
                    row[j] += grad * value
            for d, row in zip(delta, model["output"]):
                for j, value in enumerate(h + [1]):
                    row[j] += d * value
        baseline += 0.01 * (future - baseline)
    return model


def evaluate(policy, trials):
    catches = strikes = 0
    expenditure = 0
    for seed in range(trials):
        env = Encounter(900000 + seed)
        while True:
            action = policy(env.observe())
            _, done, caught, cost = env.step(action)
            catches += caught
            strikes += action == 2
            expenditure += cost
            if done:
                break
    return dict(encounters=trials, catches=catches, misses=strikes-catches,
                energy_per_catch=round(expenditure / catches, 3) if catches else None)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--episodes", type=int, default=6000)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()
    model = train(args.episodes, args.seed)
    output = Path(__file__).with_name("pike_policy.json")
    output.write_text(json.dumps(model, indent=2), encoding="utf8")
    model = json.loads(output.read_text(encoding="utf8"))
    def learned(x):
        _, probabilities = forward(model, x)
        return max(range(len(ACTIONS)), key=probabilities.__getitem__)
    for name, policy in [("rules", rules), ("learned", learned)]:
        print(name, evaluate(policy, 500))
