*By completing this PR sufficiently, you help us review it quicker and keep the release notes honest.*

### Checklist
1. [ ] A title in the commit format, `type(scope): Summary` (*eg. "fix(camera_aos): Keep the lens where it was after a big photo"*)
2. [ ] Related links below (*eg. the issue this PR closes*)
3. [ ] Relevant documentation updated - header comments, README - if the change touches what they say
4. [ ] `tools/host_tests.sh` and `tools/check_layers.sh` run, and green; `tests/target` built when `components/` changed
5. [ ] The branch is named for the change: `feat/`, `fix/`, `hotfix/`, `docs/`, `refactor/`, `test/`, `chore/`
6. [ ] If a module was used: which module and Dot, the firmware on each chip, and what was written to them, said in the issue first

*This entire section above can be deleted if all items are checked.*

-----------
## Description of Change
Please describe the proposed change and its impact.

## Test Scenarios
Please describe on what hardware and software this was tested, and how. Say which results were measured and which were read in the code.

(*eg. host suites, 300 cases; tests/target on v6.1; module KDCMR001 v1.0 on a Dot, upright and turned round, 14.3 fps measured, a photo saved and opened*)

## Related links
Please provide links to related issues, PRs etc.

(*eg. Closes #number of issue*)
