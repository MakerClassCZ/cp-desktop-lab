# Timeouts stay constants; no test waits one out

The port's safety timeouts (the control API's per-connection IO timeout and connection limit,
`cpapi`'s launch timeout, the HCI host's bounded waits) are small fixed constants, and they are
not made configurable so that tests can trigger them quickly. A test that can only show such a
guard by sitting through its timeout is not kept: the guard is a line or two, rarely changes, and
the seconds every run would pay for it, or the configuration surface a knob would add for a slow
network nobody has, cost more than the regression it might catch.

**Considered options:** an environment variable or a command-line option per timeout, so a test
could use milliseconds (rejected: configuration that exists only for tests); a slow test tier
behind a flag (rejected: it hides the cost instead of removing it).
