# Contributing

Use English for first-party code, documentation, tests and pull requests.
Report a reproducible problem with the firmware revision, hardware and host OS,
expected behavior, observed behavior and steps. Remove credentials, private
addresses, peer identities, recordings and other personal data from examples.
For vulnerabilities, use the [private reporting procedure](SECURITY.md).

Keep changes focused. Read the [interaction contract](docs/interaction.md)
before changing the UI and the [engineering standards](docs/engineering.md)
before changing firmware or a public interface. Preserve upstream attribution
and pinned dependencies. Keep reusable diagnostics with their tests and
usage instructions. Store prototypes, raw device captures, private recordings
and machine-specific scripts in an ignored local workspace.

Follow the [build guide](firmware/README.md) to resolve pinned dependencies,
then run the checks relevant to the change from the repository root:

```sh
python3 tools/check_repository.py
python3 tools/generate_control_api.py --check
python3 tools/check.py
python3 tests/ui/run.py
```

The first two commands check first-party text, links and generated API metadata.
The last two exercise host regressions and native UI rendering, respectively.
Run a target build for firmware changes. In the pull request, state which
checks passed and which behavior needs physical verification. Add regression
coverage when existing tests miss the changed behavior.
