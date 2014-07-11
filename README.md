res_freeze_check
================

res_freeze_check is a simple asterisk module to help detect some common asterisk
freezes.

Compilation dependencies
========================

* asterisk-dev


Installing
==========

make
make install


Installing from a remote machine
================================

./buildh makei


Usage
=====

CLI
---

Check if asterisk is deadlocked

```
freeze check
```


Enable/Disable locking commands

```
freeze {enable,disable}
```

Lock/Unlock the global channel container

```
freeze channel {lock,unlock}
```

AMI
---

Check if asterisk is deadlocked

```
Action: FreezeCheck
```

Asterisk response

```
Response: {Success,Fail}
```
