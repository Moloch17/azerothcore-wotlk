"""**The planner school** (.agents/plans/planner-school; docs/forge/reference/school.md): the route planner trained
on its own, offline and supervised, against the sim's labels -- the true traversability classes and route
cost-to-go of both crop tiers -- before a bot steers by its waypoints.

- `record`: the recorder in the learner's rollout (config `school.*`): samples of the map crops, the labels, the sim's
  route scalars and the memory flag, sharded .npz files under var/animus-forge/shared/school/<run_name>/.
- `data`: the shards as a streaming dataset; the decoded planes, goal planes, targets and masks of a batch.
- `model`: `SchoolPlanner` (a two-tier U-Net) and `VinPlanner` (the value iteration network, more iterations).
- `train`: the trainer and its metrics beside two baselines on the same batches, Dijkstra over the crop's codes
  (what the sim plans today) and the oracle over the true classes; `python -m animus.school.train`.
- `synthetic`: a generator of samples in the recorder's format over random rooms, the pipeline's smoke.
"""
