"""The learner's tests.

    pytest -q              the fast suite (the default; `slow` tests are left out by pyproject's addopts)
    pytest -q -m slow      only the slow ones: multi-process ranks and end-to-end training runs against a fake sim
    pytest -q -m ""        everything

Run them in the dev container (apps/forge/python/.venv): the host's python has no torch.
"""
