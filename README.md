xivo-res-freeze-check
=====================

res_freeze_check is a simple asterisk module to help detect some common asterisk
freezes.

Once loaded, the module check at regular interval if asterisk is deadlocked or not.

It currently only check if the lock on the global channels container can be
acquired or not: if it can't, then it considers asterisk is deadlocked and abort
the process.


Compilation dependencies
========================

* asterisk-dev


Installing
==========

```
make
make install
```


Installing from a remote machine
================================

```
./buildh makei
```


Usage
=====

CLI
---

Enable/Disable locking commands

```
freeze {enable,disable}
```

Lock/Unlock the global channel container (for testing purpose)

```
freeze channel {lock,unlock}
```

