---
name: Bug report
about: Create a report to help us improve
title: ''
labels: ''
assignees: ''
---

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behaviour, including the ttyd command line used on the
server side and the ttyd-client command line used on the client side:

```
ttyd -W bash
ttyd-client -d 31 localhost:7681
```

**Expected behaviour**
A clear and concise description of what you expected to happen.

**Environment:**
 - ttyd version (`ttyd --version`), including how it was installed
 - ttyd-client version (`ttyd-client --version`), including how it was built
 - OS and architecture of both ends
 - Terminal emulator used by the client
 - Relevant logs: run the client with `-d 31` and attach the output
